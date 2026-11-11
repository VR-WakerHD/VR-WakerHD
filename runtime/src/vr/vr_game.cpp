// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// VR mode, game thread: see vr_game.h and docs/VR_ARCHITECTURE.md.
//
// WWHD layouts used here (decomp/wwhd_src headers, offsets of the HD build):
//   camera_class (camera_draw 024FFC40): eye +0xDC, center +0xE8, up +0xF4 (cXyz), bank +0x100 (s16)
//   fopAc_ac_c: current.pos +0x314, current.angle.y +0x322 (Link's heading), eyePos +0x37C
//   daPy_lk_c: mpEquipItemModel +0x4440, mCameraInfoIdx +0x69BC, mSwordTopPos +0x3E4,
//     m36C4 (sword root) +0x72CC, m36D0 (previous tip) +0x72D8, m36DC (previous root) +0x72E4,
//     dCcD_Cps mAtCps[3] +0x7B1C (0x138 each): cCcD_ObjAt mSPrm +0x00 (bit 0: attack set), mRPrm +0x04
//     (bit 0: hit), mHitObj +0x08; dCcD_GObjAt mVec +0x7C; cM3dGCps +0x118: start +0, end +0xC
//   dComIfG_play_c (0x1046F0B0): mCameraInfo[1] +0x4858 (0x34 each), mCameraAttentionStatus +0x08
//   J3DModel: mpMtxBlock +0x2C -> world matrices (3x4 row-major) +0x10, count (u16) +0x2C; flags (u16) +4,
//     | 0x10 marks the matrices changed (J3DModel::setAnmMtx in HD: the skin is rebuilt from them)
//   daPy_lk_c::mpCLModel +0x448 (Link's body); arm joints 6/7/8 (left upper arm, forearm, hand), 10/11/12 (right)
//   dComIfG_play_c +0x5134: the stage name ("sea_T" title, "Name" file select)
// Movement: Link turns the stick into a world angle with dCam_getControledAngleY (024F8018), the camera's
// controlled angle (held while the stick is pushed so Link does not spiral as the follow camera swings round).
// In VR gameplay that angle is the head's heading (controlled_angle_hook), the stick itself is untouched.
// Arms: daPy_lk_c::jointCB1 (02403150) runs right after mpCLModel->calc() in Link's execute (the game's own foot
// IK writes joint matrices there); everything after it (hand positions, the sword and hands models) reads them,
// so the arms posed after it (link_arms) move the skin, the hands and what they hold.
// The sword's attack is the game's own (daPy_lk_c::setSwordAtCollision builds mAtCps from the blade's
// previous and current positions while a cut is active): a physical swing presses B, so the game chooses,
// animates and times the cut with its damage, cooldowns, combos, sounds and enemy reactions; while the
// game's attack is set, the three capsules are moved to the tracked blade's sweep before the collision
// check (dCcS::Move runs in the scene's draw, after every actor's execute).
#include "vr_game.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "aspect.h"
#include "gfx/renderer.h"
#include "gfx/vulkan/motion_history.h"
#include "gfx/display_modes.h"
#include "overlay/overlay.h"
#include "overlay/text_entry.h"
#include "input_map.h"
#include "guest_addr.h"
#include "mods/camera_mouse.h"
#include "mods/item_icons.h"
#include "mods/story_item_buttons.h"
#include "runtime.h"
#include "screenshot.h"
#include "vr.h"
#include "vr_arm.h"
#include "vr_bindings.h"
#include "vr_camera.h"
#include "vr_head.h"
#include "vr_ik.h"
#include "vr_locomotion.h"
#include "vr_swing.h"
#include "vr_sword.h"
#include "vr_baton.h"
#include "vr_item_wheel.h"
#include "vr_item_wheel_draw.h"
#include "vr_wrist_hud.h"
#include "vr_laser.h"
#include "vr_test_env.h"
#include "vr_bow.h"
#include "vr_sail_view.h"
#include "vr_bud_view.h"

namespace interp { float pass_t(); uint64_t logic_steps(); int output_fps(); }
namespace gx2 { uint64_t swaps_in_flight(); }  // (gx2_core.cpp: swaps queued, not yet executed)

namespace vr::game {
namespace {
constexpr uint32_t kCamEye = 0xDC, kCamCenter = 0xE8, kCamUp = 0xF4, kCamBank = 0x100;
constexpr uint32_t kPos = 0x314, kHeading = 0x322, kShapeAngleY = 0x32A, kEyePos = 0x37C;
// Link's facing as the HD game keeps it: his current.angle.y and shape_angle.y, and the d_a_player statics' copies of
// both that the game restores into him at every update (0240D130; WWHD_LINK_WARP and the editor's fly camera move
// them too). Setting the actor's alone was undone at his next step.
const uint32_t kKeptAngleY = GD(0x1046CD12), kKeptShapeAngleY = GD(0x1046CD0A);
void set_link_facing(uint32_t link, int16_t facing) {
    st16(link + kHeading, uint16_t(facing));       // current.angle.y (the way he moves)
    st16(link + kShapeAngleY, uint16_t(facing));   // shape_angle.y (the way his model faces)
    st16(kKeptAngleY, uint16_t(facing));
    st16(kKeptShapeAngleY, uint16_t(facing));
}
// (VR fork) his position across his execute: daPy_lk_c::execute starts with current.pos = l_debug_keep_pos (kept from the
// end of the execute before), so a move written to current.pos alone before his logic was undone. Room scale's walk
// and the turn about his eyes (room_scale_move) never moved him; both are written to both now, as his facing is
const uint32_t kKeptPos = GD(0x1046CD48);  // l_debug_keep_pos (cXyz)
void set_link_xz(uint32_t link, float x, float z) {
    stf32(link + kPos, x);
    stf32(link + kPos + 8, z);
    stf32(kKeptPos, x);
    stf32(kKeptPos + 8, z);
}
constexpr uint32_t kEquipModel = 0x4440, kCameraInfoIdx = 0x69BC, kCurProc = 0x65F0;
constexpr uint32_t kWaterY = 0x6A28;  // mWaterY: the water's surface height where he is (mods.cpp's swim effect)
constexpr uint32_t kKeptEquipActor = 0x6594;  // mActorKeepEquip.mActor (+0x6590 the keep, +4 the actor)
constexpr uint32_t kKeptGrabActor = 0x65A4;   // mActorKeepGrab.mActor (+0x65A0 the keep, +4 the actor)
// Link carries something over his head (a pot, a barrel, a rock: the grabbed actor with its carry flag,
// fopAcM_checkCarryNow), not the stick he holds as a weapon (kept as grabbed too)
bool carries_overhead(uint32_t link) {
    constexpr uint32_t kActorStatus = 0x2E0, kCarryNow = 0x2000;
    const uint32_t actor = link ? ld32(link + kKeptGrabActor) : 0;
    return actor && actor != ld32(link + kKeptEquipActor) && (ld32(actor + kActorStatus) & kCarryNow);
}
// a bomb Link carries (fpcNm_BOMB_e 0x126, fpcNm_Bomb2_e 0x127): held in the hand and thrown with it in first person
// (docs/VR_HELD_ITEMS.md, "Bombs in the hand"), not hidden and not over his head as other carried things
constexpr uint32_t kActorName = 0x8;  // base_process_class's profile name (fpcM_GetName)
uint32_t carried_bomb(uint32_t link) {
    if (!carries_overhead(link)) return 0;
    const uint32_t actor = ld32(link + kKeptGrabActor);
    const int16_t name = int16_t(ld16(actor + kActorName));
    return name == 0x126 || name == 0x127 ? actor : 0;
}
constexpr uint32_t kBokoModel = 0x3B4;        // daBoko_c::mpModel: a weapon picked up from an enemy (the stick)
constexpr uint32_t kModelBaseMtx = 0xC8;      // J3DModel::mBaseTransformMtx (3x4, as a joint matrix)
// Link's held weapon's model: his own (mpEquipItemModel), or, holding a weapon picked up from an enemy (the stick,
// mEquipItem 0x101), that weapon actor's own model. It is never his mpEquipItemModel: his setItemModel places the
// actor's model at his left hand each step (lHand x a fixed offset, about 62 units out), and the actor follows it
// (daBoko_c::procCarry). Reading only mpEquipItemModel, the VR sword path never saw the stick
uint16_t game_item(uint32_t link);  // (below: the held item as the game has it, also while his draw has changed it)
// the Deku Leaf, and the open leaf the game holds while he glides on it (its held item 0x102 then, its own model: seen in
// a headset log, 2026-11-03)
constexpr uint16_t kItemDekuLeafOpen = 0x102;
inline bool deku_leaf_item(uint16_t item) { return item == kItemDekuLeaf || item == kItemDekuLeafOpen; }
// steps a picked-up weapon (the stick) has been held: after kStickSettleSteps its geometry is kept (measure_held)
int g_stickHeldSteps = 0;
constexpr int kStickSettleSteps = 15;  // (half a second: the pickup's animation over)
uint32_t held_model(uint32_t link) {
    if (game_item(link) != kItemEnemyWeapon) return ld32(link + kEquipModel);
    const uint32_t actor = ld32(link + kKeptEquipActor);
    return actor ? ld32(actor + kBokoModel) : 0;
}
// the yaw of Link's own frame (vr_ik.h link_frame_angle): the way he moves, or on a ledge the way his model faces
float link_frame_heading(uint32_t link) {
    return s16_to_radians(link_frame_angle(ld32(link + kCurProc), int16_t(ld16(link + kHeading)), int16_t(ld16(link + kShapeAngleY))));
}
// the models the game attaches to Link's hands besides mpEquipItemModel (found at run time by their roots on his
// hand joints): the sword's hilt (with the blade, on his left hand) and the shield (on his right hand)
constexpr uint32_t kHiltModel = 0xCF8, kShieldModel = 0xE80;
// the Master Sword's glow (mpSwordModel1, item models at +0xD90, +0x3BE8): the game copies the blade's base matrix into
// it each step (daPy_lk_c's setItemModel) and calculates it in Link's draw (updateDLSetLight); the light-saber emitter
// of the Chance mode and the soup follows its joint
constexpr uint32_t kSwordGlowModel = 0x4978;
constexpr uint32_t kSwordTop = 0x3E4, kSwordRoot = 0x72CC, kSwordPrevTop = 0x72D8, kSwordPrevRoot = 0x72E4;
constexpr uint32_t kAtCps = 0x7B1C, kCpsStride = 0x138, kCpsGeometry = 0x118, kAtVec = 0x7C;
constexpr uint32_t kPlayCameraInfo = 0x4858, kCameraInfoSize = 0x34, kAttentionStatus = 0x08;
constexpr uint32_t kCloseCameraStatus = 0x20;  // dCamAttnStts_00000020_e: Link's draw hides head, hair, hat
constexpr float kStepSeconds = 1.0f / 30.0f;
constexpr uint32_t kBodyModel = 0x448, kMtxBlock = 0x2C, kMtxBlockFlags = 4, kMtxBlockMatrices = 0x10;
constexpr uint32_t kMatricesChanged = 0x10;
constexpr uint32_t kPlayStage = 0x5134;
constexpr int kHeadJointIndex = 15;      // Link's head joint (body model)
// game units from the head joint forward to between his eyes until the game's eye point gave his own (eye_forward_sample)
constexpr float kEyeForwardDefault = 4.0f;
constexpr int kArmJoints[2][3] = {{6, 7, 8}, {10, 11, 12}};  // Link's left and right: upper arm, forearm, hand
constexpr int kClavicleJoints[2] = {5, 9};                    // their parents (checked at run time: a short bone)

Vec3 ld_vec(uint32_t a) { return {float(ldf32(a)), float(ldf32(a + 4)), float(ldf32(a + 8))}; }
void st_vec(uint32_t a, Vec3 v) { stf32(a, v.x); stf32(a + 4, v.y); stf32(a + 8, v.z); }
uint32_t play() { return GD(0x1046F0B0); }
// Link conducts with the Wind Waker: the game's metronome (dComIfGp_setMetronomeOn, play +0x5BD1), set by
// daPy_lk_c::procTactWait_init and cleared at the song's end, a cancel or a talk (gfx/gamepad_ui_state.cpp reads it
// too). While it is on, the sticks and hands play the Wind Waker (vr_baton.h, docs/VR_WIND_WAKER.md)
constexpr uint32_t kPlayMetronome = 0x5BD1;
bool conducting() { return ld8(play() + kPlayMetronome) != 0; }
constexpr uint8_t kWindWakerItem = 0x22;  // the Wind Waker's item number (the save's and the wheel's)
// normal play for the view: also while Link conducts, which the game shows with a camera of its own (an event to
// aspect::gameplay_camera); the view stays as chosen, first person or third (docs/VR_WIND_WAKER.md, "The view")
// and a conversation in first person (docs/VR_CUTSCENE_TEXT.md, "Conversations in first person", vr.firstPersonTalk):
// the view stays at Link's eyes, his head hidden and his arms on the controllers, instead of the game's camera; the
// text box is held in the world (talk_text_anchor). Third person keeps the game's camera for it, as for every event
bool talk_first_person();
bool play_view() { return aspect::gameplay_or_conducting() || talk_first_person(); }
// Link looks through the telescope (player status 0 TELESCOPE_LOOK, play +0x5CD8 bit 0x200000; docs/VR_TELESCOPE.md)
constexpr uint32_t kPlayPlayerStatus0 = 0x5CD8, kTelescopeLook = 0x200000;
// the camera view (docs/VR_CONTROLS.md, "Third-person view"): first person (the head at Link's eyes, his head hidden)
// or the game's own third-person camera with the head looking around it, toggled by a right stick click; normal play
// only (events, cutscenes, menus have their own camera either way)
// (test captures: WWHD_VR_TEST_THIRD_PERSON=1 starts in third person)
std::atomic<bool> g_thirdPerson{[] { const char* e = std::getenv("WWHD_VR_TEST_THIRD_PERSON"); return e && *e == '1'; }()};
bool third_person() { return g_thirdPerson.load() && play_view(); }
// (the settings copied only during a conversation: play_view runs on every draw of every model)
bool talk_first_person() { return vr::active() && !g_thirdPerson.load() && aspect::talk_camera() && config().firstPersonTalk; }
ClickToggle g_viewToggle;
// the view mode (vr_sail_view.h, docs/VR_SAILING_VIEWS.md): the one state the right stick's click changes (under
// g_mutex), first or third person and, while sailing, the boat's three views. g_thirdPerson is only its copy for the
// gates that read it without the lock: true in every mode but first person (Link's head shown, no motion controls)
ViewCycle g_view{g_thirdPerson.load() ? ViewMode::ThirdPerson : ViewMode::FirstPerson};
// a Baba Bud's third-person view (vr_bud_view.h): switched there from first person, switched back here (under g_mutex)
BudView g_budView;
std::atomic<bool> g_boatView{false};  // (one of the boat's views: the Red Lion's or Free Roam; the UI's reads: vr_ui.cpp)
std::atomic<bool> g_freeRoamView{false};  // (the Free Roam Camera: Link not drawn, link_hidden; read by his draw's hook)
// (pad read, under g_mutex) the right stick aims an item in one of the boat's views: the Free Roam Camera's slide and
// the neck view's turn wait meanwhile (CameraOverride)
bool g_boatAimStick = false;
void view_mode_changed(const char* why) {
    g_thirdPerson = g_view.third_person();
    g_boatView = boat_view_mode(g_view.mode);
    g_freeRoamView = g_view.mode == ViewMode::FreeRoam;
    LOG("[vr] view: %s%s%s", view_mode_name(g_view.mode), why ? " " : "", why ? why : "");
}
bool telescope_view() {
    const bool on = vr::active() && (ld32(play() + kPlayPlayerStatus0) & kTelescopeLook) != 0;
    static bool was = false;
    if (on != was) {
        was = on;
        LOG("[vr] telescope view %s", on ? "begins" : "ends");
    }
    return on;
}
// Link aims the Picto Box or the Deluxe Picto Box (docs/VR_PICTO_BOX.md): player status 1 PICTO_BOX_AIM (play +0x5CDC
// bit 8), set by daPy_lk_c::procScope_init, the telescope's own procedure (its other mode: TELESCOPE_LOOK). The same
// first-person subject camera and the same zoom (the left stick's Y narrows its field of view) as the telescope
constexpr uint32_t kPlayPlayerStatus1 = 0x5CDC, kPictoBoxAim = 0x8;
bool picto_aim() { return (ld32(play() + kPlayPlayerStatus1) & kPictoBoxAim) != 0; }
bool picto_view() {
    const bool aim = picto_aim();
    static bool was = false;
    if (aim != was) {
        was = aim;
        LOG("[vr] Picto Box view %s (%s)", aim ? "begins" : "ends", vr::active() ? "VR" : "not in VR");
    }
    return vr::active() && aim;
}
// the telescope or a Picto Box: the subject camera with its zoom, the head aiming it (the magnification, and the
// gates that keep the sticks and hands to the item while it is used)
bool subject_zoom_view() { return telescope_view() || picto_view(); }

std::mutex g_mutex;  // everything below: camera draw, Link's execute and the pad filter may differ in thread

// Link, sampled at each logic step (the camera blends between the last two like the drawn model)
struct LinkStep {
    uint32_t link = 0;
    uint64_t step = ~0ull;
    Vec3 pos, prevPos;
    Vec3 roomPush;         // game units: the part of pos - prevPos that was the room-scale move (g_room), not the game's
    float heading = 0;     // radians, game convention
    float prevHeading = 0; // the step before's (the lean-back limit turns with his drawn model: head_in_game)
    bool headingSeeded = false;
    float eyeHeight = 110; // height of Link's eyes above his feet (game units), held against his idle sway
    float waterY = -INFINITY;  // the water's surface where he is (mWaterY), this step; -infinity: none read (head_in_game)
    float turnPrev = 0, turnCur = 0;  // g_head.turn_total() at the last two steps (frame_correct: the body turned since)
    // filtered horizontal offset from his feet to his eyes: his head joint (forward of his feet when he leans or runs)
    // plus the eyes' distance in front of it; without it the camera sat behind his head, his body in front of the view
    Vec3 eyeOffset;
    // the eye point (offset and height) of the step before, blended to this step's as Link's position is (anchor):
    // both change once a step, and drawn unblended they moved the view in 30 Hz steps (docs/VR_FIRST_PERSON_CAMERA.md,
    // "Steady between steps")
    Vec3 prevEyeOffset;
    float prevEyeHeight = 110;
    bool eyeBlendSeeded = false;
    Vec3 headToEye;  // this step's eye point less his head joint: his animated head's facing
    bool eyeOffsetLogged = false;
    Settled<float> eyeHeightHeld;
    // standing in place: the eye height is held against his animations (link_executed); `heightFollow` while it follows
    // as before after a posture or a lasting change until caught up, `heightAway` the seconds his eyes have been far
    bool heightFollow = false;
    float heightAway = 0, heightPeak = 0;
    uint32_t heightPeakProc = 0;
    Settled<Vec3> eyeOffsetHeld;  // in Link's frame (x right, z forward): turning him turns it at once
    // the eye point held while his body turns to follow the head, then glided to his eyes (link_executed)
    bool eyeHold = false, eyeGlide = false;
    Vec3 eyeHoldPoint;
    bool heightSeeded = false, offsetSeeded = false;  // the first measurement of a Link is taken as it is
    Settled<float> eyeForward;     // his eyes in front of his head joint, from the game's eye point (vr_camera.h)
    bool forwardSeeded = false, forwardLogged = false;
    // diagnostics: how far the camera's base moved while Link stood still (should be 0)
    float stillDrift = 0, stillHeight = 0;
    int stillSteps = 0;
    bool valid = false;
} g_link;

// The first-person VR head (vr_head.h, docs/VR_HEAD_CONTROLLER.md): the one owner of the tracking space's place in the
// game world (the mapping), the room-scale origin, the calibration, the turns and the head pose each frame is drawn
// with (g_mutex). The rest of this file reads it through the aliases below and changes it only through its methods
// (the aliases are const: a write elsewhere does not compile)
HeadController g_head;
const WorldMapping& g_map = g_head.mapping();
const RoomScale& g_room = g_head.room();
constexpr float kLeanBackMetres = 0.08f;  // (head_in_game) how far behind his eyes the view may lean
// (diagnostics, the 10 s input summary) the head's farthest lean from the view's anchor (m), re-anchors after events,
// reads whose position the runtime only estimated, hand targets beyond Link's reach (count, farthest, units)
float g_maxLean = 0;
uint32_t g_reanchors = 0, g_estimatedReads[2] = {}, g_outOfReach[2] = {};
float g_maxOutOfReach[2] = {};
// the body's facing (vr_ik.h BodyFollow, tracking space): the player's torso for the arms and swings, and the way Link
// faces while he stands (link_executing); the game's own heading for Link while it turns him (filter_pad)
BodyFollow g_body;
// lock-on in first-person play: the view's base brings the target in front of where the head looked (vr_ik.h
// LockOnView, docs/VR_BODY_FOLLOW.md "Lock-on")
LockOnView g_lockOn;
bool g_spinActive = false;  // a spin attack's view turn under way (its start's facing and shoulders taken)
// the yaw of the line between Link's shoulders (clavicle origins) as his body was last drawn, before anything is hidden
// (frame_correct); NaN when not seen
std::atomic<float> g_drawnShoulders{NAN};
// the direction of the game's sword tip from Link in a spin attack (link_executed); NaN otherwise
std::atomic<float> g_gameSwordYaw{NAN};
float g_spinTurned = 0;      // (diagnostics) radians his shoulders turned in it
bool g_spinFromShoulders = false;
bool g_spinBySword = false;
float& g_bodyYaw = g_body.yaw;
float g_cameraHeading = 0;    // the game camera's own heading (its logic), for the stick
bool g_haveCameraHeading = false;
Turner g_turner;
// Smooth turning, every frame (docs/VR_CONTROLS.md, "Smooth turning every frame"). The controllers are read where the
// game reads its pad, once a logic step (30 a second: the log's "300 pad reads" in 10 s), and the turn was applied
// there: at 120 degrees a second the view stood still for two frames of three and jumped 4 degrees on the third, while
// the head's own turns are read for each frame (reported in the headset: the stick's turn jittery and blurred, the
// head's smooth). The pad read now decides whether the stick may turn (filter_pad_controllers); each frame's camera
// (CameraOverride) reads the stick again and turns by it over the time between the frames' predicted display times,
// the headset's own clock the head's pose is predicted for: each frame shown turns by the same share (the game
// thread's own time between its frames varied by a few milliseconds: 1.59 degrees on some frames, 1.33 meant), and a
// stick pushed or let go changes the speed from that frame on, not at the next pad read. Snap turns stay at the pad
// read: one step each
struct SmoothTurn {
    bool allowed = false;  // the right stick turns the view (not in the wheel, conducting, aiming, third person...)
    std::chrono::steady_clock::time_point read{}, lastFrame{};
    XrTime lastDisplay = 0;
} g_smoothTurn;
constexpr float kSmoothTurnStale = 0.1f;  // seconds: a stick older than this (the pad not read: a pause) does not turn
// when the game last read the pad (filter_pad_controllers, at its 30 Hz steps): while it does, the controllers are read
// again every frame (CameraOverride), so the hands follow them at the headset's rate, not the steps'
std::chrono::steady_clock::time_point g_padReadAt{};
RecenterGesture g_gesture;
SwingDetector g_swing;
SwingDetector g_offSwing;            // the off hand's swings (the boomerang's downward throw)
FlapDetector g_leafFlap;             // the left hand's quick swings up or down (the Deku Leaf)
uint64_t g_throwFrom = 0, g_throwTo = 0;  // logic steps during which an off-hand swing holds the boomerang's button
HitLedger g_ledger;
PhysicalSword g_physical;
uint32_t g_physicalLink = 0;  // Link whose capsules the physical sword armed (unarmed before his next execute)
uint64_t g_bFrom = 0, g_bTo = 0;   // logic steps during which a swing holds B
bool g_bThrust = false;            // that swing was a thrust: the stick held forward with B (thrust_press)
bool g_bHammer = false;            // that swing was made with the Skull Hammer: its own button, never B
bool g_padZL = false, g_padZLShield = false, g_padA = false;
// The lock-on taken again (asked for, 2026-11-05: "camera lost tracking ... we should always track and be facing the
// enemy"): the game let go of its target 6 times in a play test with ZL still held, and with Z held it takes no new
// one. From g_lockLostAt (the step it let go, ZL held) ZL is let go for one pad read kRelockFirst steps on, and again
// every kRelockEvery steps while none came, kRelockTries times: each new press locks onto the nearest enemy, as a
// press of Z does
uint64_t g_lockLostAt = 0;
int g_relockTries = 0;
constexpr uint64_t kRelockFirst = 2, kRelockEvery = 15;
constexpr int kRelockTries = 3;  // (diagnostics) ZL in the last pad read, and held by the shield's lock
bool g_bParry = false;             // that swing was made in a parry's window: A, the game's parry (docs/VR_CONTROLS.md)
bool g_bVertical = false;          // that swing was a downward one: locked on, the game's vertical slash (vertical_press)
// (2026-11-05, a player's log: 9 of 21 locked-on downward swings did nothing, most 1 to 3 steps after another attack
// began, the hand only 0.09-0.12 m down: the hand coming back from a thrust or a side cut, taken for a swing while Link
// was still in his attack, and fast real swings the game was still too busy for) A full swing within kPhantomSteps of
// the last is the hand's return: no press. A press that started no sword move is queued: pressed again once his attack
// is over, for kQueueSteps at most (swing_queue_step)
constexpr uint64_t kPhantomSteps = 5;   // (0.17 s: no chop is made in that time after another full swing)
constexpr uint64_t kQueueSteps = 20;    // (0.67 s: a queued swing still pressed, then dropped)
uint64_t g_lastFullSwing = 0;           // the step of the last full swing of the sword hand
uint32_t g_moveSerial = 0;              // sword moves the game has begun (link_executed)
struct QueuedSwing {
    bool active = false;
    uint64_t until = 0;
    uint32_t serial = 0;
    bool thrust = false, vertical = false, comboReset = false;
    int side = 0;
} g_queued;
int g_bSide = 0;                   // that side swing's side (+1 to the right): locked on, the game's side cut
// Parry attacks in first person (docs/VR_CONTROLS.md, "Parry attacks"). The game parries (daPy_lk_c::changeSpecialBattle)
// on A with Link locked on to an enemy whose parry window is open, the action prompt the parry's (do status 0x1A); a
// swing pressed B, never A, and the prompt is not shown in VR. Read on the game thread once a step (link_executed)
constexpr uint32_t kLockOnActor = 0x65B0;  // daPy_lk_c::mpAttnActorLockOn
constexpr uint32_t kRollCutCentre = 0x7314;  // daPy_lk_c::m370C: the point the parry roll's cut turns about (procBtRollCut)
// dComIfG_play_c::mDoStatus (the A button's action) at play + 0x5BB7 in HD, as setSpecialBattle (023EC478) stores the
// parry's 0x1A and mods/play_context.h has it; 0x4917 before (the GameCube's place), never the parry's or the jump
// attack's, so a swing never parried (a play test's 3 parries all by A, no window seen; 2026-11-05)
constexpr uint32_t kPlayDoStatus = 0x5BB7;
constexpr uint8_t kDoStatusParry = 0x1A;    // dActStts_PARRY_e
// (2026-11-10, a player's logs and "they always expected A to be pressed first": 5 parries began from a swing just
// before the window and 2 from a flick of the sword hand, none from A) the sword no longer starts a parry: a swing's
// start or flick in the window, a full swing in it, a swing just before it (buffered or held for it). Only A starts it;
// a swing in the parry's move still makes the finishing cut (parry_cut_wait)
constexpr bool kParryBySwing = false;
constexpr uint64_t kParryCutWait = 45;      // (steps: 1.5 s) the finishing cut's wait for the player's swing at most
// (asked for: easier to land) the enemy's parry window (fopEn_enemy_c mBtStartFrame .. mBtEndFrame, its attack's frames,
// within mBtMaxDis) as Link's two checks read it in first person: kParryEarly frames earlier, kParryLate later, its reach
// kParryReach times as far; and a swing up to kParryBuffer steps before the window opens parries as it opens
constexpr uint32_t kBtMaxDis = 0x3B4, kBtStartFrame = 0x3B8, kBtEndFrame = 0x3BC;
// (12, 12 and 2 since 2026-11-05, asked for: "make it easier to initiate the start of a parry attack"; 8, 8, 1.5 before)
constexpr float kParryEarly = 12.0f, kParryLate = 12.0f, kParryReach = 2.0f;
constexpr uint64_t kParryBuffer = 24;  // (0.8 s; 18 before 2026-11-05)  // (0.6 s: the held swing below, before the window, included)
// Locked-on sword moves in first person (docs/VR_CONTROLS.md, "Locked-on sword moves"; Orca's training, 2026-11-04):
// each swing is the game's own move, chosen as on the GamePad by the stick with B (side swing: stick to its side;
// downward: middle, the vertical slash; thrust: forward), or A for the jump attack (a downward swing begun with the
// sword hand above the head, while the game offers it: do status 0x51). Repeated thrusts and vertical slashes each start
// the combo again (daPy_lk_c::m34C4, its count: the 4th of a combo is a finisher, which Orca refused as a thrust).
// A swing just before the locked-on enemy's parry window (kParryHoldFrames of its attack's frames) presses nothing
// and parries as the window opens (Orca: "No! You can only perform a parry by ... pressing A")
constexpr uint32_t kComboCount = 0x68E0;      // daPy_lk_c::m34C4
constexpr uint32_t kBtNowFrame = 0x3C0, kBtAttackType = 0x3C4;
constexpr uint8_t kDoStatusJumpAttack = 0x51;
constexpr float kParryHoldFrames = 12.0f;
bool g_bJump = false;          // that swing presses A: the jump attack
bool g_bComboReset = false;    // that swing's press starts the combo again (a thrust or vertical slash, locked on)
bool g_swingHigh = false;      // the swing in progress began with the sword hand above the head
constexpr float kJumpAttackAbove = 0.15f;  // metres over the head the hand starts the jump attack's swing
inline bool locked_on(uint32_t link) { return link && ld32(link + kLockOnActor) != 0; }
// the locked-on enemy is about to open its parry window (its attack under way, the window not yet open in Link's
// widened check)
inline bool parry_window_near(uint32_t link) {
    const uint32_t e = link ? ld32(link + kLockOnActor) : 0;
    if (!e || ld8(e + kBtAttackType) == 0) return false;
    const float now = float(ldf32(e + kBtNowFrame)), opens = float(ldf32(e + kBtStartFrame)) - kParryEarly - 1.5f;
    return std::isfinite(now) && std::isfinite(opens) && now < opens && now >= opens - kParryHoldFrames;
}
inline bool parry_move_proc(uint32_t proc) { return proc >= 0x5D && proc <= 0x64; }  // BT_JUMP .. BT_VERTICAL_JUMP_LAND
struct ParryState {
    bool ready = false;          // first person, locked on, the parry's prompt up: a swing parries
    int readySteps = 0;          // (the cue's pulses)
    bool inMove = false;         // Link in a parry's move (jump, roll, leap, their cuts)
    uint64_t moveStep = 0;       // the step it began
    uint64_t lastSwing = 0;      // the last swing's step, its start or its end (any weapon state)
    uint64_t lastFull = 0;       // the last full swing's step (the buffered parry)
    bool waitLogged = false;
} g_parry;
// the Skull Hammer (vr_sword.h, docs/VR_SKULL_HAMMER.md): during g_bFrom..g_bTo a swing presses the hammer's own item
// button instead, with the left stick sideways for a side swing; its windup is skipped once, for the swing pressed at
// g_hammerPressStep
float g_hammerStick = 0;
// VR archery (vr_bow.h): the arrow put on the bow with the off hand, shot with either trigger
BowMachine g_bow;
uint64_t g_hammerPressStep = ~0ull;
bool g_hammerSkipped = true;
Blade g_prevGameBlade;
uint64_t g_prevBladeStep = ~0ull;
xr::InputState g_input;
std::chrono::steady_clock::time_point g_lastInput{};
// movement: the heading "forward" on the stick means (game convention), and the constant between the game's
// controlled angle and the camera's view heading (measured while the stick is released)
float g_wantedHeading = 0;
bool g_haveWanted = false, g_stickPushed = false;
// the left stick pushed backwards (more than 100 degrees from forward): standing, Link is left to the game, which turns
// him round before he walks (body_follows_head)
bool g_stickBack = false;
// walking backwards (docs/VR_CONTROLS.md): Link stands facing the way you face while the stick points back, and VR moves
// him (back_slide) by g_backSlide: the stick from where the head looks, tracking space, length up to vr.moveSpeed
bool g_backStep = false;
// moving while aiming an item in first person (the bow, hookshot, boomerang, grappling hook: aiming_item): VR moves Link
// the stick's way by the same slide (back_slide), the game given no stick. Its aims walked him slowly or not at all
// (measured: the grappling hook's aim 0.00 a step, another 3.2, against 17 running), and moving while aiming was asked
// for with videos (2026-11-04: "freely move and freely aim")
bool g_aimStep = false;
Vec3 g_backSlide{};
int16_t g_angleOffset = 0;
bool g_haveAngleOffset = false;
// arms: tracking weight per Link arm (0 animation .. 1 controller), and whether this Link's arms check out
float g_armWeight[2] = {0, 0};
float g_detachWeight = 0;  // vr.detachedArms eased: 0 the arms held to Link's shoulders .. 1 detached
uint32_t g_armsCheckedLink = 0;
bool g_armsOk = false;
bool g_claviclesOk = false;
bool g_handsLogged[2] = {};
Vec3 g_hingeLocal[2];  // each elbow's hinge axis in its upper arm's frame (pose_arm_natural)
int g_jointSet[2] = {0, 1};  // the joint set (kArmJoints) of Link's left and right arm, checked at run time
Vec3 g_elbowPole[2] = {{0, -1, 0}, {0, -1, 0}};
Settled<Vec3> g_bodyHold;  // Link's head joint in his own frame, held against the run cycle (first person)
bool g_bodySeeded = false;
uint32_t g_bodyLink = 0;
Settled<Vec3> g_shoulderHold[2];  // each upper arm's root in Link's frame, held against the run cycle's bob
bool g_shoulderSeeded[2] = {};
uint32_t g_shoulderLink = 0;  // smoothed: the elbow never pops to another side between steps
// The wrist displays (docs/VR_WRIST_HUD.md): the head of the pass being drawn (first-person play) and each display's
// frame on its wrist at the last two logic steps (drawn blended between them, like his model)
Pose g_passHead;
bool g_passHeadValid = false;
std::atomic<uint64_t> g_passSerial{0};  // counts the passes (camera overrides): the wrist displays' per-pass cache
// (diagnostics, WWHD_VR_TRACE_LAG=1) each hand's IK target at the last two logic steps and its scale from the
// controller's offset to the target: how far the drawn hand (from the steps) trails the controller at each frame
// Also what the arms re-aim from every frame (frame_correct): the two steps' targets and the controller's orientation
// (in the game world) they were aimed with
struct LagTrace {
    uint64_t step = ~0ull;
    Vec3 prev, cur;
    Quat prevRot, curRot;
    float k = 1;
    bool valid = false;
    Quat handRot;            // the hand as posed at the step curRot is from (game world)
    bool haveHand = false;
} g_lag[2];
// each pass (the camera override): how far each controller's wrist target has moved, and how far the controller has
// turned, since what the drawn arm blends were aimed at (by controller side; serial: the pass, g_passSerial)
struct FrameHand {
    bool valid = false;
    Vec3 delta;
    Quat turn;
    // the hand's turn now: as posed at the last step, turned by the controller's turn since that step. Not from the
    // drawn blend of two steps' hands: when Link's facing jumped (a full turn of the body: 136 degrees in a step,
    // measured) that blend swung the hand's turn through the turn mid-step while the controller held still, and the
    // hand and the sword in it flipped about (reported in the headset)
    Quat handNow;
    bool haveHandNow = false;
    uint64_t serial = ~0ull;
} g_frameHand[2];
// each pass (Link's body drawn): each of his hands' rigid move (by Link arm joint set: 0 his left, 1 his right), for his
// hands model and what he holds (frame_correct)
struct HandMove {
    bool valid = false;
    Vec3 from, to;
    Quat rot;
    uint64_t serial = ~0ull;
} g_handMove[2], g_foreMove[2];  // (g_foreMove: each forearm's, for the Power Bracelets on it)
// A picked-up weapon (the stick) held rigidly in the hand as it is drawn (frame_correct). It had been blended between
// steps on its own, as every model is: its joints, 60 units and more out along it, travelled straight between two
// steps' places while the hand travelled along another line, so it drifted against the hand between the steps and
// snapped back at each, most while Link moved or the view turned (reported in the headset: the stick jitters in the
// hand). Now each pass places it from the hand as drawn (blended, then moved to its controller):
// - g_drawnHand: each Link hand as drawn this pass (per arm joint set), recorded by his body's correction;
// - g_weaponGrip: the weapon's base in the frame of the hand holding it, at the step (link_draw_begin), and its root
//   joint in its base's frame (weapon_drawing).
struct DrawnHand { Quat rot; Vec3 pos; uint64_t serial = ~0ull; } g_drawnHand[2];
// One piece (docs/VR_HELD_ITEMS.md): the sword (blade and hilt) and the shield rigidly in the hand as it is drawn each
// pass: their root in the frame of the hand holding them (arm joint set), taken from the step's placement
// (link_draw_begin), so each pass places them from that hand (frame_correct_pose) as the picked-up stick is. Moved
// with the hand's move since the step instead, they drifted against it between steps and snapped back at each
// (reported with a video: shaky, the hand coming loose from the handles)
struct HandLock { uint32_t model = 0; int set = 0; Quat rot; Vec3 pos; bool valid = false; bool both = false; };
enum { kLockBlade, kLockHilt, kLockShield, kLockCount };
HandLock g_locks[kLockCount];
DrawnHand g_drawnHandPrev[2];  // each hand as drawn the pass before g_drawnHand's (its last move: lock_target)
struct LockPlaced { int fresh = 0, carried = 0, none = 0; } g_lockPlaced;  // passes (the input summary's log)
constexpr float kFrameHandCut = 200.0f;  // game units: a controller this far from its arm's blend is a cut, not a lag
int g_frameHandFar = 0;          // passes a hand was not re-aimed: its controller kFrameHandCut or more from its blend
float g_frameHandFarMax = 0;
// Link's hands closed in first person whatever the grips (apply_hands). Off since 2026-11-04 (asked for: "when Link has
// nothing in his hands his hands should be open"): an empty hand is open, a pressed grip closes it, a held item keeps
// its grip
constexpr bool kFistsClosed = false;
constexpr bool kShieldPlaceKept = false;  // the shield's place in the hand taken once and kept (off: it lost its level)
uint32_t matrices_of(uint32_t model, uint16_t& count, uint32_t* block);  // (below)
Joint read_joint(uint32_t mtx, int j);
// the model's root now in the frame of hand joint `hand` of `body` (its set: 0 his left arm, 1 his right)
void lock_to_hand(int slot, uint32_t model, uint32_t body, int hand) {
    HandLock& l = g_locks[slot];
    l.valid = false;
    uint16_t n = 0;
    const uint32_t mtx = model ? matrices_of(model, n, nullptr) : 0;
    if (!mtx || !body) return;
    const Joint root = read_joint(mtx, 0), h = read_joint(body, hand);
    if (!finite(root.pos) || !finite(h.pos)) return;
    l.model = model;
    l.set = hand == kArmJoints[1][2] ? 1 : 0;
    l.rot = normalize(conjugate(h.rot) * root.rot);
    l.pos = rotate(conjugate(h.rot), root.pos - h.pos);
    l.valid = true;
    l.both = false;
}
// the model's root held by both hands (the Deku Leaf's glide): from the midpoint of his two hands, turned with his left
// hand (the one the game attaches items to); each pass then places it from both hands as drawn (frame_correct_pose)
void lock_to_both_hands(int slot, uint32_t model, uint32_t body) {
    HandLock& l = g_locks[slot];
    l.valid = false;
    uint16_t n = 0;
    const uint32_t mtx = model ? matrices_of(model, n, nullptr) : 0;
    if (!mtx || !body) return;
    const Joint root = read_joint(mtx, 0), h0 = read_joint(body, kArmJoints[0][2]), h1 = read_joint(body, kArmJoints[1][2]);
    if (!finite(root.pos) || !finite(h0.pos) || !finite(h1.pos)) return;
    const Vec3 mid = (h0.pos + h1.pos) * 0.5f;
    l.model = model;
    l.set = 0;
    l.rot = normalize(conjugate(h0.rot) * root.rot);
    l.pos = rotate(conjugate(h0.rot), root.pos - mid);
    l.valid = true;
    l.both = true;
}
struct WeaponGrip {
    bool valid = false, rootValid = false;
    int set = 0;              // the arm (joint set) holding it
    Quat rot, rootRot;
    Vec3 pos, rootPos;
} g_weaponGrip;
struct WeaponPlaced { int fresh = 0, stale = 0, none = 0; } g_weaponPlaced;  // passes (the input summary's log)
// this pass's turn of Link's body by the stick since the step drawn (g_head.turn_total), for what he holds away from his
// hands
struct BodyTurn { bool valid = false; Vec3 pivot; Quat rot; uint64_t serial = ~0ull; } g_bodyTurn;
// each hand joint's straight-wrist direction in its own frame (per kArmJoints set): the forearm's direction (elbow to
// wrist) as the idle pose holds the hand, i.e. the hand in line with the forearm
Vec3 g_wristStraight[2] = {{0, 0, 1}, {0, 0, 1}};
// each hand's "up" axis, in its own frame, when it is held level (per kArmJoints set; level_hand_up, from his idle pose):
// an empty hand's roll is set from it, so it follows the controller and not his body's turn or run cycle
Vec3 g_handLevelUp[2];
// each hand's fixed offset from its controller (per kArmJoints set; controller_to_hand, from his idle pose): an empty
// hand is held rigidly to its controller through it (ArmStyle::rigid)
Quat g_ctrlToHand[2];
bool g_ctrlToHandOk[2] = {};

// ---- held items (docs/VR_HELD_ITEMS.md)
// The side (0 left, 1 right) whose controller holds the sword: the one rule for the sword's buttons (openxr_session
// sync_input), swing detection, haptics, the swing's collision and the hand the held item is drawn in.
int dominant_side(const Config& c) { return c.leftHanded ? 0 : 1; }
uint16_t game_item(uint32_t link);  // (below: the held item as the game has it)
// the side holding and aiming an item (the off hand for the grappling hook and the hookshot: vr_sword.h off_hand_item)
int aim_side(const Config& c, uint16_t item) {
    const int fixed = fixed_item_side(item);  // the bow, hookshot and grappling hook: the same hands either way
    if (fixed >= 0) return fixed;
    return off_hand_item(item) ? 1 - dominant_side(c) : dominant_side(c);
}
// The bow's aim (tracking space): with the arrow on the bow, the arrow itself (asked for in the headset: the laser
// spawned with the arrow, from its middle, along it): the game's arrow actor, kept on Link's left hand
// (daArrow_c::setKeepMatrix), along its model's +Z (the way it flies: its shape_angle as the game sets it from its
// speed), from kArrowMiddle behind its position (the arrowhead: its flight collision runs back from there). Before, where
// the arrow's hand (the left, vr_sword.h fixed_item_side) points; `onArrow` says which
constexpr float kArrowMiddle = 30.0f;  // game units from the arrowhead to the arrow's middle (an estimate)
bool g_bowOnBow = false;  // the arrow on the bow: the bow's button held for the player (vr_bow.h)
// The arrow on the bow as drawn (frame_correct, each pass; game units): the game keeps it on Link's left hand, where it
// holds the bow, its arrowhead at the bow; in VR the bow is in the right hand and the left draws, so it stood out of the
// drawing hand at that hand's angle (reported with a video: not flush or parallel with the bow). Now it lies along the
// line from the drawing hand through the bow hand, its nock (the model's origin) at the drawing hand (the arrow slides
// through the bow as the hand draws back) and its head at least kHeadPastBow past the bow; the aim and the laser follow
// that line
struct BowLine { bool valid = false; Vec3 nock, dir, head; uint32_t arrow = 0; } g_bowLine;  // (arrow: the actor drawn along it)
std::mutex g_bowLineMutex;
uint32_t g_bowDropArrow = 0;  // (g_bowLineMutex) the arrow let go before drawn back: it falls (arrow_kept)
// the arrow falling out of the hand (arrow_fall_step): which, steps so far, its fall speed, the way it was facing
struct ArrowFall { uint32_t arrow = 0; int steps = 0; float vy = 0; Vec3 forward; } g_arrowFall;
constexpr int kArrowFallSteps = 45;           // a second and a half at most
constexpr float kArrowFallGravity = 2.5f;     // game units a step, each step
constexpr float kArrowFallMax = 40.0f;        // its fastest fall a step
constexpr float kArrowFallForward = 1.5f;     // game units a step along the bow
constexpr float kArrowLength = 2.0f * kArrowMiddle;  // game units, nock to head (an estimate)
constexpr float kHeadPastBow = 5.0f;                 // game units the head stays past the bow hand at least
constexpr uint32_t kArrowModel = 0x3B0;              // daArrow_c::mpModel
bool bow_aim(Vec3& from, Vec3& dir, bool* onArrow = nullptr) {
    if (onArrow) *onArrow = false;
    const xr::HandState& arrowHand = g_input.hand[0];
    if (!g_input.valid || !arrowHand.active) return false;
    const uint32_t l = g_link.link;
    const uint32_t arrow = g_bowOnBow && l && g_link.valid ? ld32(l + kKeptEquipActor) : 0;  // mActorKeepEquip: the arrow
    if (arrow) {
        BowLine line;
        {
            std::lock_guard<std::mutex> lock(g_bowLineMutex);
            line = g_bowLine;
        }
        const float x = s16_to_radians(int16_t(ld16(arrow + kShapeAngleY - 2))),  // shape_angle.x, .y
                    y = s16_to_radians(int16_t(ld16(arrow + kShapeAngleY)));
        // the arrow as drawn along the bow (g_bowLine, this arrow's), else as the game keeps it
        const bool drawn = line.valid && line.arrow == arrow;
        const Vec3 along = drawn ? line.dir : Vec3{std::sin(y) * std::cos(x), -std::sin(x), std::cos(y) * std::cos(x)};
        const Vec3 at = (drawn ? line.head : ld_vec(arrow + kPos)) - along * kArrowMiddle;
        const Vec3 p = to_xr_point(g_map, at), d = rotate(conjugate(game_rotation(g_map)), along);
        // (the line between the hands pointing back from the bow, the drawing hand past it: not the aim, which would
        // turn Link round to face behind you; the bow hand's own pointing instead, below. Reported with a video,
        // 2026-11-04: aiming and moving with the bow out, he turned away and the aim no longer followed)
        const xr::HandState& bowHand = g_input.hand[1];
        const bool backwards = drawn && bowHand.active && length(d) > 0.5f &&
                               dot(normalize(d), rotate(bowHand.aim.orientation, {0, 0, -1})) < 0.0f;
        if (backwards) {
            from = bowHand.aim.position;
            dir = rotate(bowHand.aim.orientation, {0, 0, -1});
            return finite(from) && finite(dir);
        }
        if (finite(p) && finite(d) && length(d) > 0.5f) {
            from = p;
            dir = normalize(d);
            if (onArrow) *onArrow = true;
            return true;
        }
    }
    from = arrowHand.aim.position;
    dir = rotate(arrowHand.aim.orientation, {0, 0, -1});
    return finite(from) && finite(dir);
}
// The game attaches what Link holds (mpEquipItemModel) to his left hand (joint 8: Link is left-handed). Measured at
// each execute, from the game's own state of the step:
struct Held {
    uint32_t model = 0;          // the model measured
    int gameHand = -1;           // the body joint it is attached to (8 or 12); -1: not in a hand (sheathed, stowed)
    // gliding on the Deku Leaf: its handle (the leaf's joint nearest the hand in the game's glide pose) in the leaf's own
    // frame, so the leaf is placed with its handle in the tracked hand (place_held's glide branch)
    Vec3 leafHandle;
    int leafHandleJoint = -1;
    float leafHandleGap = 0;     // (diagnostics) that joint's distance from the hand in the game's pose
    bool sword = false;          // a sword (a model the game's sword tip and root move with)
    Vec3 tipLocal, baseLocal;    // the game's sword tip and blade root in the model's root frame
    // the Skull Hammer (vr_sword.h): its head's centre, its shaft (towards the head) and its face's direction, in the
    // model's root frame, from its base matrix as the game places it (valid: measured)
    bool hammer = false, hammerKnown = false;
    Vec3 headLocal, shaftLocal, faceLocal;
    Vec3 lastRoot;               // the model's root at the last measurement (movement tells a live blade)
    bool haveLast = false;
    Vec3 wroteTip, wroteRoot;    // what the tracked swing last wrote to the game's sword tip and root
    // the game's attachments of this step (measured after Link's execute, before any draw moved them): the held item
    // in the game's hand's frame, the hilt in the blade's frame, the shield in its hand's frame. The draw places from
    // these every pass: Link is drawn several times a step, and after the first pass the models are where the last
    // pass put them (or where frame interpolation put them back), so placing relative to where they are flickered.
    Quat itemRot, hiltRot, shieldRot;
    int shieldAcross = 0;  // the shield's own axis that points across Link's body (its face)
    int shieldFlip = 1;    // the axis its mirrored frame is flipped on (mirror_flip_axis: neither its face nor its up)
    Vec3 itemPos, hiltPos, shieldPos;
    int itemAcross = 2;    // the held item's own axis that points across Link's body (its mirror's flip axis)
    Vec3 acrossVotes;      // per item axis, summed: how much it points across his body in the animation's pose
    bool hiltOnHand = false, shieldOnHand = false;
    // where the draw last put the hilt's and the shield's roots: found there at the next step, the game did not
    // attach them again, and the attachments measured before still hold
    Vec3 hiltPlaced{1e9f, 0, 0}, shieldPlaced{1e9f, 0, 0};
    // a picked-up weapon: its handle (the game's blade root) in each of Link's hands' frames (the game's hand measured,
    // the other mirrored), and where its draw last put its root joint, and the frame (its base's, tracked) it put it in
    Vec3 handleLocal[2];
    bool handleKnown = false;
    Vec3 rootPlaced{1e9f, 0, 0};
    Quat placedRot;
    Vec3 placedAt;
    // where place_weapon_base last put the weapon's base (link_executed): found there at the next step, the game did
    // not set it again, and the grip measured before still holds
    Vec3 baseWritten{1e9f, 0, 0};
} g_held;
// The shield model's own face and top (vr_ik.h shield_axes), learnt per shield model from the game's own pose of its
// hand: Link's right hand as the animation posed it (link_arms, before the arm IK), with the attachment measured. Not
// from where the shield is drawn: on Link's right hand in right-handed play that is the tracked sword hand, and
// choosing the axes from it each step flipped the shield upside down as the sword moved (reported in the headset).
struct ShieldLearn {
    uint32_t model = 0;
    Vec3 outward, up;  // per shield axis, summed: along the direction away from Link's body, along world up
    int votes = 0;
    ShieldAxes axes;
} g_shield;
Joint g_animShieldHand;  // Link's right hand as the animation posed it this step (link_arms)
bool g_animShieldHandFresh = false;
Joint g_animHand[2];     // Link's left and right hands as the animation posed them this step (link_arms)
bool g_animHandFresh = false;
// The bow's string following the drawing hand (docs/VR_AIMING.md, asked for in the headset): the bow model's joint the
// string bends at, found in the game's own drawn pose as the joint nearest his drawing hand (measure_held); each pass
// while the arrow is on the bow it is put at the drawing hand as drawn (frame_correct_pose), so the string comes back
// with the hand instead of the game's fixed full draw
// (two: the string's top and bottom halves, each its own joint as near the hand: 6 and 11 of the Hero's Bow's 12,
// measured; with only the top moved, the bottom half stayed straight, reported in the headset)
struct BowString { uint32_t model = 0; int joint = -1, joint2 = -1; } g_bowString;
constexpr float kBowStringPair = 3.0f;  // game units: a second joint this near the nearest's distance is the other half
constexpr float kBowStringFind = 40.0f;  // game units: the drawn pose's string joint this near his drawing hand

// The sword's grip: the sword's root frame in the frame of the hand holding it, per Link arm (joint set 0: his left,
// 1: his right). The game's hand (his left) is measured each step from the game's own attachment; the other hand's is
// that grip mirrored across Link's body with his rig's mirror convention (g_mirrorSign), so the gripping hand shape
// closes around the hilt the same way on either hand. The hand is the reference: the arm IK turns the sword hand so
// this grip holds while the blade follows the controller, and the draw and the swing's collision place the sword
// from the hand.
struct Grip {
    Quat rot;      // sword root frame in the hand's frame
    Vec3 pos;      // sword root in the hand's frame
    bool valid = false;
};
Grip g_grip[2];
// hand-local axes of Link's left hand mirrored across his body map onto his right hand's axes times these signs
// (his rig: measured from the animation pose of both hands, logged once); 0: not known yet
Vec3 g_mirrorSign;
bool g_mirrorKnown = false;
// a grip mirrored onto the other hand: hand-local vectors x -> diag(sign) x; the sword's axis that points across
// Link's body is flipped back (a sword is symmetric about its blade plane) so the result is a rotation again
Grip mirror_grip(const Grip& g, Vec3 sign, int flipAxis) {
    Grip out;
    mirror_held(g.rot, g.pos, sign, flipAxis, out.rot, out.pos);
    out.valid = true;
    return out;
}
int g_gripFlipAxis = 2;
uint32_t matrices_of(uint32_t model, uint16_t& count, uint32_t* block = nullptr) {
    const uint32_t b = model ? ld32(model + kMtxBlock) : 0;
    const uint32_t m = b ? ld32(b + kMtxBlockMatrices) : 0;
    count = b ? ld16(b + 0x2C) : 0;
    if (block) *block = b;
    return count && count <= 64 ? m : 0;
}
Joint read_joint(uint32_t mtx, int j) {
    float m[12];
    for (int i = 0; i < 12; ++i) m[i] = float(ldf32(mtx + j * 48 + i * 4));
    return joint_from_matrix(m);
}
bool same(Vec3 a, Vec3 b) { return length(a - b) < 1e-4f; }
// the bow's place in the hand from the game's aiming pose (measure_held; docs/VR_AIMING.md, "The bow's turn in the hand")
struct BowHold { bool valid = false; Quat rot; Vec3 pos; } g_bowHold;
constexpr int kBowSettle = 5;
void measure_held(uint32_t link) {
    uint16_t n = 0, bn = 0;
    const uint32_t model = held_model(link);
    const uint32_t mtx = model ? matrices_of(model, n) : 0;
    const uint32_t body = matrices_of(ld32(link + kBodyModel), bn);
    if (model != g_held.model) {
        const Vec3 wroteTip = g_held.wroteTip, wroteRoot = g_held.wroteRoot;
        g_stickHeldSteps = 0;
        g_held = {};
        g_held.model = model;
        g_held.wroteTip = wroteTip;
        g_held.wroteRoot = wroteRoot;
    }
    const int lastHand = g_held.gameHand;
    const bool hadHilt = g_held.hiltOnHand, hadShield = g_held.shieldOnHand;
    g_held.gameHand = -1;
    g_held.hiltOnHand = g_held.shieldOnHand = false;
    // the held item at each change (the stick and other picked-up weapons: which of the three outcomes; WWHD_VR_TRACE_HELD
    // for every step)
    {
        static uint32_t loggedModel = ~0u;
        static uint16_t loggedItem = 0xFFFF;
        const uint16_t item = ld16(link + 0x69B0);
        if (model != loggedModel || item != loggedItem) {
            loggedModel = model;
            loggedItem = item;
            const uint32_t actor = item == kItemEnemyWeapon ? ld32(link + kKeptEquipActor) : 0;
            LOG("[vr] held: item 0x%X (%s), model %08X%s", item, melee_item(item) ? "a tracked weapon" : "not a weapon",
                model, item == kItemEnemyWeapon ? (actor ? " (a picked-up weapon's actor)" : " (no weapon actor)") : "");
        }
    }
    if (!body || bn <= 12) return;
    // the shield on its hand (Link's right), in that hand's frame
    {
        uint16_t sn = 0;
        const uint32_t sm = matrices_of(ld32(link + kShieldModel), sn);
        if (sm && sn <= 32) {
            const Joint sr = read_joint(sm, 0), h = read_joint(body, kArmJoints[1][2]);
            if (hadShield && same(sr.pos, g_held.shieldPlaced)) {
                g_held.shieldOnHand = true;  // as the draw left it: the attachment measured before
            } else if (finite(sr.pos) && length(sr.pos - h.pos) < 3.0f) {
                g_held.shieldOnHand = true;
                g_held.shieldRot = normalize(conjugate(h.rot) * sr.rot);
                g_held.shieldPos = rotate(conjugate(h.rot), sr.pos - h.pos);
                g_held.shieldAcross = across_axis(sr.rot, heading_right(s16_to_radians(int16_t(ld16(link + kHeading)))));
                g_held.shieldFlip = mirror_flip_axis(g_held.shieldAcross, up_axis(sr.rot, g_held.shieldAcross));
            }
            // its own axes, from the game's pose of its hand (summed, so a moment of an animation does not decide)
            const uint32_t shieldModel = ld32(link + kShieldModel);
            if (g_shield.model != shieldModel) g_shield = ShieldLearn{shieldModel};
            if (g_held.shieldOnHand && g_animShieldHandFresh && finite(g_animShieldHand.pos)) {
                const Quat animShield = normalize(g_animShieldHand.rot * g_held.shieldRot);
                Vec3 out = g_animShieldHand.pos - ld_vec(link + kPos);
                out.y = 0;
                out = length(out) > 1.0f ? normalize(out) : heading_right(s16_to_radians(int16_t(ld16(link + kHeading))));
                const Vec3 ax = rotate(animShield, {1, 0, 0}), ay = rotate(animShield, {0, 1, 0}),
                           az = rotate(animShield, {0, 0, 1});
                g_shield.outward = g_shield.outward + Vec3{dot(ax, out), dot(ay, out), dot(az, out)};
                g_shield.up = g_shield.up + Vec3{ax.y, ay.y, az.y};
                const ShieldAxes was = g_shield.axes;
                g_shield.axes = shield_axes(g_shield.outward, g_shield.up);
                if (++g_shield.votes == 30 || (g_shield.votes > 30 && (was.face != g_shield.axes.face ||
                                                                       was.top != g_shield.axes.top ||
                                                                       was.faceSign != g_shield.axes.faceSign ||
                                                                       was.topSign != g_shield.axes.topSign)))
                    LOG("[vr] shield: its face is its axis %c%d, its top %c%d (from %d steps of the game's pose)",
                        g_shield.axes.faceSign < 0 ? '-' : '+', g_shield.axes.face, g_shield.axes.topSign < 0 ? '-' : '+',
                        g_shield.axes.top, g_shield.votes);
            }
        }
    }
    g_animShieldHandFresh = false;
    const bool animHandFresh = g_animHandFresh;
    g_animHandFresh = false;
    if (!mtx || n > 32) return;
    // a picked-up weapon from its base matrix (J3DModel +0xC8), which his setItemModel set from his hand in this execute.
    // Its joint matrices are the weapon actor's own, calculated in its execute after his: here they still held where the
    // tracked draw put them last pass, and measuring the grip from them fed that placement back into the next one
    // (the stick wandered round the view, reported in the headset)
    const bool enemyWeapon = ld16(link + 0x69B0) == kItemEnemyWeapon;
    // A picked-up weapon (the stick) is rigid: its geometry in its own frame (its handle in the hand, its tip and base,
    // the axis a mirrored grip flips) is measured while it settles in the hand and then kept until another is held.
    // Measured again at every step it took on the game's walking and holding animation, and where the measured blade
    // crossed an axis the frame built from it turned half round: the stick drifted, and flipped round in the hand,
    // its base 40 units away, for a step or a few (reported in the headset: it was displaced now and then)
    g_stickHeldSteps = enemyWeapon ? g_stickHeldSteps + 1 : 0;
    const bool stickKept = enemyWeapon && g_stickHeldSteps > kStickSettleSteps && g_held.handleKnown;
    Joint root = read_joint(mtx, 0);
    // a picked-up weapon by its base matrix: the grip is the base's in the game's hand, and its draw moves its joints
    // from the base's frame to the tracked one (weapon_drawing)
    if (enemyWeapon) root = read_joint(model + kModelBaseMtx, 0);
    if (enemyWeapon && lastHand >= 0 && same(root.pos, g_held.baseWritten)) {
        g_held.gameHand = lastHand;  // the base as place_weapon_base left it: what was measured before holds
        return;
    }
    // the hand it is attached to (a little slack for the hand it was on: a moment of an animation does not drop it)
    for (const auto& arm : kArmJoints) {
        const float dist = finite(root.pos) ? length(root.pos - read_joint(body, arm[2]).pos) : 1e9f;
        if (dist < 3.0f || (arm[2] == lastHand && dist < 6.0f)) g_held.gameHand = arm[2];
    }
    // a weapon picked up from an enemy: the game places it from his left hand (setItemModel, CL_JNT_CL_LHANDA_e 8),
    // its root far out along the weapon, so it is that hand's whenever its model is placed
    if (ld16(link + 0x69B0) == kItemEnemyWeapon && finite(root.pos)) g_held.gameHand = kArmJoints[0][2];
    // gliding on the Deku Leaf (daPyProc_FAN_GLIDE_e 0x93): the game's pose holds it in both hands, its base more than
    // 3 units from either, so it counted as in no hand (as a sword on his back): not placed at the tracked hand and
    // hidden in first person (hides_from_view). It is the nearest hand's then, placed from it as measured now (the
    // leaf up over the hand: the hand keeps the glide pose's turn, link_arms). Reported in the headset: the hands
    // were right but the glider was invisible floating down
    const bool leafGlide = deku_leaf_item(ld16(link + 0x69B0)) && ld32(link + kCurProc) == 0x93 && finite(root.pos);
    // (held by both hands: always measured on his left hand, the one the game attaches items to; the nearer hand of
    // the two switched as the hands moved, and the leaf jumped between them: reported in the headset)
    if (leafGlide) g_held.gameHand = kArmJoints[0][2];
    // its handle: the leaf's joint nearest that hand in the game's pose (reported in the headset: the leaf was seen
    // but not in his hands, its centre half a metre above them), kept in the leaf's own frame
    g_held.leafHandleJoint = -1;
    if (leafGlide && g_held.gameHand >= 0) {
        // (where both hands hold it in the game's pose: their midpoint)
        const Vec3 h = (read_joint(body, kArmJoints[0][2]).pos + read_joint(body, kArmJoints[1][2]).pos) * 0.5f;
        float best = 1e9f;
        for (int j = 0; j < int(n); ++j) {
            const Joint lj = read_joint(mtx, j);
            const float dist = length(lj.pos - h);
            if (finite(lj.pos) && dist < best) {
                best = dist;
                g_held.leafHandleJoint = j;
                g_held.leafHandle = rotate(conjugate(root.rot), lj.pos - root.pos);
            }
        }
        g_held.leafHandleGap = best;
    }
    if (g_held.gameHand < 0) return;
    // the bow's string joint, once, from the game's drawn pose (the arrow on: the game draws it to his other hand)
    // (the drawn pose's hands as link_arms last took them: measured before this step's, so never fresh here, and the
    // search never ran in the headset; a step old they are a unit or two off, near enough for the nearest joint)
    const bool animHands = finite(g_animHand[0].pos) && finite(g_animHand[1].pos);
    if (bow_in_hand(game_item(link)) && g_bowOnBow && g_bowString.model != model && (!animHands || !mtx)) {
        static uint32_t why = 0;  // (diagnostics: why it was not looked for)
        if (why != model) {
            why = model;
            LOG("[vr] bow: its string not looked for yet (the drawn pose's hands %s, its joints %s)",
                animHands ? "taken" : "not taken yet", mtx ? "read" : "not read");
        }
    }
    if (bow_in_hand(game_item(link)) && g_bowOnBow && animHands && mtx && g_bowString.model != model) {
        const Joint drawing = g_animHand[g_held.gameHand == kArmJoints[1][2] ? 0 : 1];
        int best = -1;
        float bestD = kBowStringFind;
        for (int j = 1; j < int(n); ++j) {
            const float dj = length(read_joint(mtx, j).pos - drawing.pos);
            if (finite(drawing.pos) && dj < bestD) bestD = dj, best = j;
        }
        // (diagnostics, once per bow model) its joints against both hands of the drawn pose: which is the string
        static uint32_t listed = 0;
        if (listed != model) {
            listed = model;
            std::string list;
            for (int j = 0; j < int(n); ++j) {
                char b[64];
                snprintf(b, sizeof b, " %d:%.0f/%.0f", j, double(length(read_joint(mtx, j).pos - g_animHand[0].pos)),
                         double(length(read_joint(mtx, j).pos - g_animHand[1].pos)));
                list += b;
            }
            LOG("[vr] bow: %u joints, units from his left/right hand in the drawn pose:%s", unsigned(n), list.c_str());
        }
        // the string by its shape first: its two halves' joints at one place (vr_bow.h bow_string_pair); the nearest
        // joint to the hand only for a model without such a pair (in the boat's pose that was a limb: the bow warped)
        int pairA = -1, pairB = -1;
        {
            Vec3 pos[32];
            const int count = std::min(int(n), 32);
            for (int j = 0; j < count; ++j) pos[j] = read_joint(mtx, j).pos;
            bow_string_pair(pos, count, drawing.pos, pairA, pairB);
        }
        if (pairA > 0) {
            g_bowString = {model, pairA, pairB};
            LOG("[vr] bow: its string is joint %d and %d of %u (its two halves at one place; %.1f units from the drawing "
                "hand in the game's drawn pose)", pairA, pairB, unsigned(n),
                double(length(read_joint(mtx, pairA).pos - drawing.pos)));
        } else if (best > 0) {
            int second = -1;
            float secondD = bestD + kBowStringPair;
            for (int j = 1; j < int(n); ++j) {
                const float dj = length(read_joint(mtx, j).pos - drawing.pos);
                if (j != best && dj <= secondD) secondD = dj, second = j;
            }
            g_bowString = {model, best, second};
            LOG("[vr] bow: its string is joint %d and %d of %u (%.1f and %.1f units from the drawing hand in the game's "
                "drawn pose; no two joints at one place)", best, second, unsigned(n), double(bestD),
                double(second > 0 ? secondD : 0.0f));
        }
    }
    {
        const Joint h = read_joint(body, g_held.gameHand);
        g_held.itemRot = normalize(conjugate(h.rot) * root.rot);
        g_held.itemPos = rotate(conjugate(h.rot), root.pos - h.pos);
        // the bow (docs/VR_AIMING.md, "The bow's turn in the hand"): the game holds it upright while aiming (0x94) and
        // flat or sideways in its other actions (walking with it, standing): with the hand on the controller, the bow
        // turned in the hand as the actions changed (reported with a video, 2026-11-04). Its place in the hand is taken
        // from the aiming pose, once that has settled (kBowSettle steps), and kept for every action
        {
            static uint16_t lockItem = 0;
            static int aimSteps = 0;
            const uint16_t item = game_item(link);
            if (!bow_in_hand(item)) {
                g_bowHold.valid = false;
                aimSteps = 0;
            } else {
                if (item != lockItem) g_bowHold.valid = false, lockItem = item;
                aimSteps = bow_aim_proc(ld32(link + kCurProc)) ? aimSteps + 1 : 0;  // (the land's 0x94, the boat's 0x8D)
                if (aimSteps >= kBowSettle && finite(g_held.itemPos)) {
                    if (!g_bowHold.valid) LOG("[vr] bow: its place in the hand taken from the aiming pose");
                    g_bowHold = {true, g_held.itemRot, g_held.itemPos};
                }
                if (g_bowHold.valid) {
                    g_held.itemRot = g_bowHold.rot;
                    g_held.itemPos = g_bowHold.pos;
                }
            }
        }
        // its axis across his body, by which it is mirrored onto his other hand: from the animation's pose of its hand
        // (before the arm IK), summed over the steps it is held. From where it is now, the axis was chosen again each
        // step, and with the hand on its controller a turn of that hand changed it: the Wind Waker flipped round in the
        // other hand as the left hand reached out to the left (reported in the headset)
        {
            const int set = g_held.gameHand == kArmJoints[1][2] ? 1 : 0;
            const Vec3 right = heading_right(s16_to_radians(int16_t(ld16(link + kHeading))));
            const Quat posed = animHandFresh && finite(g_animHand[set].pos) ? normalize(g_animHand[set].rot * g_held.itemRot)
                                                                               : root.rot;
            if (length(g_held.acrossVotes) == 0 || animHandFresh) {
                const Vec3 ax = rotate(posed, {1, 0, 0}), ay = rotate(posed, {0, 1, 0}), az = rotate(posed, {0, 0, 1});
                g_held.acrossVotes = g_held.acrossVotes +
                                     Vec3{std::fabs(dot(ax, right)), std::fabs(dot(ay, right)), std::fabs(dot(az, right))};
            }
            const Vec3& v = g_held.acrossVotes;
            g_held.itemAcross = v.x >= v.y && v.x >= v.z ? 0 : v.y >= v.z ? 1 : 2;
        }
        uint16_t hn = 0;
        const uint32_t hilt = ld32(link + kHiltModel);
        const uint32_t hm = hilt && hilt != model ? matrices_of(hilt, hn) : 0;
        if (hm && hn <= 32) {
            const Joint hr = read_joint(hm, 0);
            if (hadHilt && same(hr.pos, g_held.hiltPlaced)) {
                g_held.hiltOnHand = true;  // as the draw left it: the attachment measured before
            } else if (finite(hr.pos) && length(hr.pos - h.pos) < 6.0f) {
                g_held.hiltOnHand = true;
                g_held.hiltRot = normalize(conjugate(root.rot) * hr.rot);
                g_held.hiltPos = rotate(conjugate(root.rot), hr.pos - root.pos);
            }
        }
    }
    // the sword by the game's own item number (mEquipItem +0x69B0: 0x103, the sword), from its first step in the hand:
    // recognising it by its movement first took a step or two, and the sword showed in the game's hand before it
    // moved to the player's (reported in the headset). A stick or other weapon picked up from an enemy (0x101) is held,
    // tracked and swung the same way (vr_sword.h melee_item); it was left in the game's hand and could not be swung
    g_held.sword = melee_item(ld16(link + 0x69B0));
    g_held.hammer = ld16(link + 0x69B0) == kItemSkullHammer;
    // the game's grip (the sword's frame in the frame of the hand it holds it with), from its first step in the hand
    // (it needs no blade), and the other hand's mirrored from it (with the last blade axis known; vr_game.cpp
    // mirror_grip)
    if (g_held.sword) {
        const int set = g_held.gameHand == kArmJoints[1][2] ? 1 : 0;
        const Joint h = read_joint(body, g_held.gameHand);
        g_grip[set] = {normalize(conjugate(h.rot) * root.rot), rotate(conjugate(h.rot), root.pos - h.pos), true};
        g_grip[1 - set] = g_mirrorKnown ? mirror_grip(g_grip[set], g_mirrorSign, g_gripFlipAxis) : Grip{};
        // a picked-up weapon by its handle: its root is far out along it (about 62 units), where a mirrored grip's
        // small errors grew into the weapon floating beside the hand (seen in the headset); its handle is at the hand
        if (enemyWeapon && g_held.haveLast && g_mirrorKnown && !stickKept) {
            const Vec3 handle = root.pos + rotate(root.rot, g_held.baseLocal);
            g_held.handleLocal[set] = rotate(conjugate(h.rot), handle - h.pos);
            Quat unused;
            mirror_held(Quat{}, g_held.handleLocal[set], g_mirrorSign, g_gripFlipAxis, unused, g_held.handleLocal[1 - set]);
            g_held.handleKnown = finite(g_held.handleLocal[0]) && finite(g_held.handleLocal[1]) &&
                                 length(g_held.handleLocal[set]) < 30.0f;
        }
    }
    // the game's blade of this step (not the tracked one this code wrote at the last step)
    const Vec3 tip = ld_vec(link + kSwordTop), base = ld_vec(link + kSwordRoot);
    if (same(tip, g_held.wroteTip) && same(base, g_held.wroteRoot)) return;
    const float len = length(tip - base);
    // (a picked-up weapon's root is not at its handle: the game places it from his hand at an offset along it)
    const float rootSlack = ld16(link + 0x69B0) == kItemEnemyWeapon ? 250.0f : 40.0f;
    if (!finite(tip) || !finite(base) || !(len > 20.0f && len < 250.0f) || length(base - root.pos) > rootSlack) return;
    const Vec3 tipLocal = rotate(conjugate(root.rot), tip - root.pos), baseLocal = rotate(conjugate(root.rot), base - root.pos);
    // the blade's axis in the model's frame: which axis a mirrored grip flips back (below)
    if (g_held.sword && !stickKept) {
        g_gripFlipAxis = across_axis(root.rot, heading_right(g_link.heading), normalize(tipLocal - baseLocal));
    }
    if (!stickKept) {
        g_held.tipLocal = tipLocal;
        g_held.baseLocal = baseLocal;
    }
    g_held.lastRoot = root.pos;
    g_held.haveLast = true;
    // the hammer: its head's centre from the base matrix the game took the face and root from this step (the face and
    // root are the game's, measured above); its shaft runs from the root to the head, its face from the head's centre
    if (g_held.hammer) {
        float m[12];
        for (int i = 0; i < 12; ++i) m[i] = float(ldf32(model + kModelBaseMtx + i * 4));
        const Vec3 head{m[0] * kHammerHeadAlong + m[3], m[4] * kHammerHeadAlong + m[7], m[8] * kHammerHeadAlong + m[11]};
        const Vec3 headLocal = rotate(conjugate(root.rot), head - root.pos);
        const Vec3 shaft = headLocal - baseLocal, face = tipLocal - headLocal;
        if (finite(headLocal) && length(shaft) > 20.0f && length(face) > 10.0f &&
            std::fabs(dot(normalize(shaft), normalize(face))) < 0.2f) {
            const bool first = !g_held.hammerKnown;
            g_held.headLocal = headLocal;
            g_held.shaftLocal = normalize(shaft);
            g_held.faceLocal = normalize(face - g_held.shaftLocal * dot(face, g_held.shaftLocal));
            g_held.hammerKnown = true;
            if (first)
                LOG("[vr] hammer: face %.0f %.0f %.0f, head %.0f %.0f %.0f, root %.0f %.0f %.0f (its frame); shaft %.0f "
                    "long, the face %.0f from it",
                    double(tipLocal.x), double(tipLocal.y), double(tipLocal.z), double(headLocal.x), double(headLocal.y),
                    double(headLocal.z), double(baseLocal.x), double(baseLocal.y), double(baseLocal.z),
                    double(length(shaft)), double(length(face)));
        }
    }
}
// The sword's root orientation for a controller: its blade (tip from blade root, in the model's frame) along the
// controller's blade, the controller's up as the twist. The draw and the swing's collision use the same.
// The sword's base pose for a controller: where it points (OpenXR's aim pose: a thrust points it forward) at the
// grip's position. vr.gripPitchDegrees tilts the blade up from there (blade_from_grip)
Pose sword_base(const xr::HandState& h) { return {h.aim.orientation, h.grip.position}; }
// the sword's rotation as the arms were posed this step (link_arms): the drawn sword and its collision use it, so
// they follow the controller even where the hand is turned straight with the forearm (vr.swordWristStraight)
Quat g_swordRot;
bool g_swordRotValid = false;

Quat held_sword_rotation(const Pose& grip, const Config& c) {
    const Blade xrBlade = blade_from_grip(grip, c);
    const Vec3 bladeGame = normalize(to_game_dir(g_map, xrBlade.tip - xrBlade.root));
    const Vec3 upGame = to_game_dir(g_map, rotate(grip.orientation, {0, 1, 0}));
    Vec3 bladeLocal = normalize(g_held.tipLocal - g_held.baseLocal);
    Vec3 perpLocal = cross(bladeLocal, std::fabs(bladeLocal.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0});
    perpLocal = normalize(cross(perpLocal, bladeLocal));
    Vec3 perpGame = normalize(upGame - bladeGame * dot(upGame, bladeGame));
    if (g_held.hammer && g_held.hammerKnown) {
        // the hammer: its shaft along the controller (as a blade), its face away from the controller's top, so a
        // downward swing of the wrist leads with the face; turned about the shaft by vr.hammerRollDegrees
        bladeLocal = g_held.shaftLocal;
        perpLocal = g_held.faceLocal * -1.0f;
        perpGame = rotate(axis_angle(bladeGame, c.hammerRollDegrees * kPi / 180.0f), perpGame);
    }
    const Quat local = from_basis(bladeLocal, perpLocal, cross(bladeLocal, perpLocal));
    const Quat target = from_basis(bladeGame, perpGame, cross(bladeGame, perpGame));
    return normalize(target * conjugate(local));
}

bool running() { return vr::active(); }
// a facing that changes this much in one step with Link's feet still is the game turning him at once (link_executed:
// his eye point is held and glides, as for a body turn)
constexpr float kFacingJump = 20.0f * 3.14159265f / 180.0f;
// ---- the VR menu (docs/VR_MENU.md): the left stick's click opens the settings on the headset's panel, the game paused
std::atomic<bool> g_menuOpen{false};
std::atomic<bool> g_menuRequest{false};  // the click asked for it (filter_pad); opened at the next pass's start
bool g_menuClickHeld = false;  // (the click is held: one toggle per press)
// (a half-second hold to open it, 2026-10-31, was taken back the same day: in the headset a click no longer opened the
// menu at all, as far as the player could tell. One click opens it, one closes it, as before)
float g_gripMax[2] = {};  // the grips' highest values (input summary)
GripLatch g_gripLatch[2];  // each physical controller's grip closing its Link hand (docs/VR_CONTROLS.md)
// the sword hand's recent places (tracking space) and when, for its velocity (bombs thrown with it)
struct HandSample { Vec3 pos; float t = 0; };
constexpr int kHandTrail = 12;
constexpr float kThrowWindow = 0.1f;  // seconds the throwing hand's velocity is taken over
HandSample g_handTrail[kHandTrail];
int g_handTrailN = 0;
float g_handClock = 0;
// a bomb let go of (the pad's read): its throw's velocity, game units a second, applied at the game's throw
struct BombThrow { bool pending = false; uint32_t bomb = 0; Vec3 vel; uint64_t step = 0; } g_bombThrow;
constexpr uint16_t kItemBombBag = 0x31;          // dItemNo_BOMB_BAG_e: the bombs' item button
constexpr float kBombThrowGain = 3.2f;           // the hand's speed times this (the game's throw: 19 forward, 34 up a step)
constexpr float kBombLoft = 14.0f;               // game units a step added upwards: an arc, not a flat throw
constexpr float kBombMaxForward = 60.0f, kBombMaxUp = 60.0f, kBombMinUp = -20.0f;  // game units a step
constexpr float kAimStickFloor = 0.75f;          // aiming: a push past the dead zone counts from this
constexpr uint32_t kProcLeafSwing = 0x92;       // daPyProc_FAN_SWING_e: the Deku Leaf's fan swing
constexpr uint32_t kProcLeafGlide = 0x93;
constexpr float kGlideFadeSeconds = 0.45f;       // the arms into and out of the glide's pose       // daPyProc_FAN_GLIDE_e: gliding on the Deku Leaf
constexpr float kThrowSpeed = 2.2f, kThrowAcross = 1.0f;  // m/s: a throwing motion of the hand holding a bomb
constexpr uint8_t kDoStatusThrow = 0xE;          // dActStts_THROW_e: A throws what Link carries
constexpr uint64_t kCarryThrowGap = 15;          // logic steps between throwing motions of a carried thing (0.5 s)
constexpr float kBombInHand = 12.0f;             // game units out from the palm, along the controller
constexpr uint32_t kSpeed = 0x33C, kSpeedF = 0x370;  // fopAc_ac_c::speed, speedF
// the sword's state (vr_sword.h) and the shoulder draw: a draw asked for at an input read, taken at the next step
SwordMachine g_sword;
ShoulderReach g_reach;
bool g_drawRequested = false;

// ---- the item wheel (vr_item_wheel.h, docs/VR_ITEM_WHEEL.md): the off hand over its shoulder opens it, the right stick
// chooses, the hand brought forward puts the item on the item button vr.itemWheelButton, as the game's item menu does.
// Game thread (the pad filter), under g_mutex.
ItemWheel g_wheel;
Baton g_baton;  // conducting with the Wind Waker (vr_baton.h)
std::vector<WheelPageItems> g_wheelPages;  // what it shows: the save read when it opened, and again while it is open
std::vector<int> g_wheelIcons;             // item number -> icon frame (the game's table, read when it opens)
uint64_t g_wheelVersion = 1;               // its picture's contents (vr_item_wheel_draw.cpp draws it again on a change)
Pose g_wheelPose;                          // tracking space: follows the head's position, keeps the turn it opened with
float g_wheelYaw = 0;                      // the body's yaw when it opened
float g_wheelReadIn = 0;                   // s until the save is read again while it is open
bool g_wheelShown = false;                 // set_wheel_view's last visible
bool g_wheelClickWas = false, g_wheelClickUsed = false;  // the right stick click: a cancel, never also the view toggle
// the logic steps during which the wheel's choice presses its button (the forward pull takes the item out), and the
// GamePad button
uint64_t g_wheelPressFrom = 0, g_wheelPressTo = 0;
uint32_t g_wheelPressBit = 0;

// the save (dSv_info_c's player, as mods/cheats.cpp and story_item_buttons.cpp read it)
const uint32_t kSavePtr = GD(0x101F84DC);
constexpr uint32_t kSaveMaxLife = 0x20, kSaveLife = 0x22, kSaveRupees = 0x24, kSaveSelectKeys = 0x29, kSaveMaxMagic = 0x33, kSaveMagic = 0x34, kSaveItems = 0x5C,
                   kSaveGot = 0x71, kSaveArrows = 0x89, kSaveBombs = 0x8A, kSaveBeast = 0x96, kSaveBait = 0x9E,
                   kSaveReserve = 0xA6, kSaveBeastNum = 0xBC, kSaveBaitNum = 0xC4;
constexpr uint32_t kPlaySelectItems = 0x5BBB;  // play's mSelectItem[5]: the item numbers on X, Y, R as actors read them
// hdMenuItem itemForSlot's table (0262F880): 57 entries of 16 bytes, {item number, icon frame, ...}
const uint32_t kIconTable = GD(0x100E7AB8);
constexpr int kIconTableEntries = 57;

// the save of the game in play (0: none, or its values don't look like a save's)
uint32_t save_base() {
    const uint32_t b = ld32(kSavePtr);
    if (!b) return 0;
    const uint16_t max = ld16(b + kSaveMaxLife), life = ld16(b + kSaveLife);
    return max >= 12 && max <= 80 && life <= max ? b : 0;
}
WheelSave read_wheel_save(uint32_t b) {
    WheelSave s;
    for (int i = 0; i < 21; ++i) s.items[i] = ld8(b + kSaveItems + i), s.got[i] = ld8(b + kSaveGot + i);
    for (int i = 0; i < 8; ++i) {
        s.beast[i] = ld8(b + kSaveBeast + i);
        s.bait[i] = ld8(b + kSaveBait + i);
        s.reserve[i] = ld8(b + kSaveReserve + i);
        s.beastNum[i] = ld8(b + kSaveBeastNum + i);
        s.baitNum[i] = ld8(b + kSaveBaitNum + i);
    }
    s.arrows = ld8(b + kSaveArrows);
    s.bombs = ld8(b + kSaveBombs);
    s.magic = ld8(b + kSaveMagic);
    for (int i = 0; i < 3; ++i) s.buttons[i] = ld8(b + kSaveSelectKeys + i);
    return s;
}
std::vector<int> wheel_icon_frames() {
    std::vector<int> frames(256, -1);
    for (int i = 0; i < kIconTableEntries; ++i) {
        const uint32_t item = ld32(kIconTable + 16 * i), frame = ld32(kIconTable + 16 * i + 4);
        if (item < 256 && frame < uint32_t(item_icons::kGameFrames)) frames[item] = int(frame);
    }
    // the Wind Waker (0x22) is not in the table: its picture is the Quest screen's, kept after the game's frames
    // (item_icons::game_icons)
    if (frames[0x22] < 0) frames[0x22] = item_icons::kWindWakerIcon;
    return frames;
}
// the save key `key` on item button `button` (0 X, 1 Y, 2 R), as the game's item menu puts it there: the button that
// held it already takes the button's old key (hdIngameUi_c::assignItemSlot), and the play's item numbers follow
// (dComIfGp_setSelectItem: the item for the key, or none; a key that holds nothing is cleared)
void wheel_assign(int button, uint8_t key) {
    const uint32_t b = save_base();
    if (!b || button < 0 || button > 2) return;
    uint8_t before[3];
    for (int i = 0; i < 3; ++i) before[i] = ld8(b + kSaveSelectKeys + i);
    const ButtonKeys k = assign_button(before, button, key);
    for (int i = 0; i < 3; ++i) {
        if (k.keys[i] == before[i] && i != button) continue;
        st8(b + kSaveSelectKeys + i, k.keys[i]);
        uint8_t item = kNoItem;
        if (k.keys[i] != kNoItem) {
            const int at = story_item_buttons::save_item_offset(k.keys[i]);
            item = at < 0 ? kNoItem : ld8(b + uint32_t(at));
            if (item == kNoItem) st8(b + kSaveSelectKeys + i, kNoItem);
        }
        st8(play() + kPlaySelectItems + i, item);
    }
    // a mod item or the notebook on a button the game's item now holds comes off it (as after the game's own menu)
    story_item_buttons::game_assigned(before, unsigned(button));
}

// test aid (no headset needed): WWHD_VR_TEST_WHEEL=step[:page:index[:button]] logs the item wheel's pages as read from
// the save once a save is in play and, at that logic step, puts that entry (default: page 0's last item) on the button
// (default X) with the wheel's own assignment, logging the buttons' keys and the play's item numbers before and after,
// then Link's held item every 30 steps for 10 s (a scripted press of the button shows the item in use)
void test_wheel(uint32_t link) {
    // test aid (no headset needed): WWHD_VR_TEST_KEY=step:key:button[:item] puts the save key `key` (hex: the inventory
    // slot, 08 the Picto Boxes) on the button (0 X, 1 Y, 2 R) at that logic step, with the wheel's own assignment;
    // with `item` (hex: 23 the Picto Box, 26 the Deluxe Picto Box) the slot gets that item first
    if (static const char* keySpec = std::getenv("WWHD_VR_TEST_KEY"); keySpec && save_base()) {
        static bool done = false;
        unsigned long long at = 0;
        unsigned key = 0, item = 0xFF;
        int button = 0;
        if (!done && std::sscanf(keySpec, "%llu:%x:%d:%x", &at, &key, &button, &item) >= 2 && interp::logic_steps() >= at) {
            done = true;
            if (item != 0xFF && key < 21) {
                st8(save_base() + kSaveItems + key, uint8_t(item));
                st8(save_base() + kSaveGot + key, uint8_t(ld8(save_base() + kSaveGot + key) | 1));
            }
            wheel_assign(button, uint8_t(key));
            LOG("[vr] test: key %02X on button %d: the play's items X %02X Y %02X R %02X", key, button,
                ld8(play() + kPlaySelectItems), ld8(play() + kPlaySelectItems + 1), ld8(play() + kPlaySelectItems + 2));
        }
    }
    static const char* spec = std::getenv("WWHD_VR_TEST_WHEEL");
    static bool listed = false, assigned = false;
    static uint64_t assignedAt = 0;
    if (!spec) return;
    const uint32_t b = save_base();
    if (!b) return;
    unsigned long long at = 0;
    int page = 0, index = -1, button = 0;
    std::sscanf(spec, "%llu:%d:%d:%d", &at, &page, &index, &button);
    const std::vector<WheelPageItems> pages = wheel_pages(read_wheel_save(b));
    const uint64_t step = interp::logic_steps();
    // listed whenever what it reads changes (the title screen has placeholder save data; the real save comes later)
    std::string seen;
    for (const auto& pg : pages)
        for (const WheelEntry& e : pg.entries) seen += char(e.item), seen += char(e.key), seen += char(e.count & 0xFF);
    static std::string listedSeen;
    if (!listed || seen != listedSeen) {
        listed = true;
        listedSeen = seen;
        const std::vector<int> icons = wheel_icon_frames();
        LOG("[vr] item wheel test: the save offers %zu pages", pages.size());
        for (size_t p = 0; p < pages.size(); ++p)
            for (const WheelEntry& e : pages[p].entries)
                LOG("[vr] item wheel test: %s: %s (item %02X, key %02X, icon frame %d, count %d%s%s%s)", page_name(pages[p].page),
                    item_name(e.item), e.item, e.key, icons[e.item], e.count, e.empty ? ", empty" : "",
                    e.button >= 0 ? ", on a button" : "", e.detail.empty() ? "" : (", " + e.detail).c_str());
    }
    auto buttons = [&](const char* when) {
        LOG("[vr] item wheel test: %s: keys X %02X Y %02X R %02X, the play's items X %02X Y %02X R %02X, Link holds %04X", when,
            ld8(b + kSaveSelectKeys), ld8(b + kSaveSelectKeys + 1), ld8(b + kSaveSelectKeys + 2), ld8(play() + kPlaySelectItems),
            ld8(play() + kPlaySelectItems + 1), ld8(play() + kPlaySelectItems + 2), ld16(link + 0x69B0 /* mEquipItem */));
    };
    if (!assigned && step >= at && page < int(pages.size())) {
        assigned = true;
        assignedAt = step;
        const auto& entries = pages[page].entries;
        const WheelEntry& e = entries[index < 0 || index >= int(entries.size()) ? entries.size() - 1 : size_t(index)];
        buttons("before");
        wheel_assign(button, e.key);
        LOG("[vr] item wheel test: at step %llu (TV frame %llu) %s put on button %d", (unsigned long long)step,
            (unsigned long long)aspect::game_frame(), item_name(e.item), button);
        buttons("after");
    } else if (assigned && step > assignedAt && step <= assignedAt + 300 && (step - assignedAt) % 30 == 0) {
        buttons("later");
    }
}
// first-person play with motion controls: the sword's gates apply (third person plays as on the GamePad)
bool sword_gates() { return !g_thirdPerson.load() && aspect::gameplay_camera() && !subject_zoom_view() &&
                            !gfx::display_gamepad_menu_open(); }
// first-person normal play: the view at Link's eyes, the game's camera in charge (no event, menu or telescope)
bool first_person_play() {
    return running() && !g_thirdPerson.load() && aspect::gameplay_camera() && !subject_zoom_view() &&
           !gfx::display_gamepad_menu_open();
}
// Link conducts with the Wind Waker in first person: the metronome on and his action his own conducting (TactWait
// 0x9A, TactPlay 0x9B). The conducting corrections of the body (it faces where the hands are held) apply only then.
// (Not gfx::display_gamepad_menu_open: it counts the song list the game shows on the GamePad all through conducting,
// and the body kept the game's facing, which turns him round: backwards in VR, seen in the headset)
bool conducting_first_person(uint32_t link) {
    if (!link || !running() || g_thirdPerson.load() || !conducting()) return false;
    const uint32_t proc = ld32(link + kCurProc);
    return (proc == 0x9A || proc == 0x9B) && play_view();
}
// the body follows the head (docs/VR_BODY_FOLLOW.md) in first-person play while Link stands, and while he conducts
// with the Wind Waker (conducting_first_person): the game holds his facing while he conducts, and with his arms
// on the controllers they reached across his body when you faced another way (reported in the headset)
// (a re-anchor owed: a new stage, until recenter. Set by body_follows_head, cleared by recenter)
bool g_anchorOwed = false;
const char* stage_name();
bool body_follows_head(uint32_t link) {
    if (conducting_first_person(link)) return true;
    // Not before the view is anchored again after an event or in a new stage (asked for, 2026-11-05: "Link walked into
    // the room and faced the door again"): the game's camera still on (an event, the door's entry: end_game_camera and
    // recenter come on the next camera frame), or a new stage not yet anchored. Followed then, the head's yaw went
    // through the old mapping, the old room's, and turned him 180 degrees to the door on the first steps in your hands
    // ("Link turned 180 degrees at once (action 0x4)" twice before "recentred" in the log); recenter then read his model
    // facing the door and anchored the view to it
    {
        static char lastStage[9] = {};
        static uint64_t owedAt = 0;
        const char* st = stage_name();
        const uint64_t now = interp::logic_steps();
        if (st && std::strncmp(st, lastStage, 8) != 0) {
            if (lastStage[0]) {
                g_anchorOwed = true;
                owedAt = now;
            }
            std::strncpy(lastStage, st, 8);
        }
        // (never held longer than 10 s: a recentre seen before this saw the stage would leave it owed)
        if (g_anchorOwed && now > owedAt + 300) g_anchorOwed = false;
    }
    if (g_head.game_camera() || g_anchorOwed) return false;
    // (the stick pushed backwards: the game turns him round to walk that way. Faced to the head again every step, he
    // never finished the turn and stood still: moving backwards did nothing, reported in the headset)
    if (g_stickBack) return false;
    return first_person_play() && body_follow_proc(ld32(link + kCurProc));
}
uint32_t g_trackHeld[2] = {}, g_trackLost[2] = {};  // tracking diagnostics (input summary)
uint32_t g_heldPlaced = 0;  // draw passes that placed a held item (input summary)

// the time the frame being built will be shown: the render thread's last predicted time plus the frames
// between (the game builds the next frame while the previous one is presented). The render thread predicts at each
// swap (xrWaitFrame): when the game goes on before the render thread has executed its last swap (PERF-07a: the swap's
// GX2DrawDone is skipped), that prediction is a frame older, and each swap still queued is one more frame between
std::atomic<uint64_t> g_cameraAhead[5] = {}, g_cameraAheadChanges{0};  // (take_camera_ahead)
XrTime display_time(int* aheadOut = nullptr) {
    static const int ahead = [] {
        const char* e = std::getenv("WWHD_VR_PREDICT_FRAMES");
        return e ? std::max(0, std::min(4, std::atoi(e))) : 1;
    }();
    auto& s = session();
    // (the swaps still queued, counted up to WWHD_VR_PREDICT_QUEUED_MAX, default 3: each makes the render thread's last
    // prediction a frame older, so the default keeps counting them all. A streaming runtime (Virtual Desktop, Link)
    // that already predicts far ahead can be tried with fewer, against the "camera ahead" counts in the log)
    static const uint64_t queuedMax = [] {
        const char* e = std::getenv("WWHD_VR_PREDICT_QUEUED_MAX");
        return uint64_t(e ? std::max(0, std::min(3, std::atoi(e))) : 3);
    }();
    const int frames = std::min(4, ahead + int(std::min<uint64_t>(gx2::swaps_in_flight(), queuedMax)));
    if (aheadOut) *aheadOut = frames;
    // each frame is shown for the headset's refreshes per drawn frame: 1 at 90 fps on 90 Hz, 2 at 45 fps, 3 at 30 (and
    // 1.5 on average at 60 fps). Counted as one period a frame, the pose at 45 fps was predicted a refresh (11 ms) before
    // the frame is shown, and the view lagged the head by that while it moved (jitter, reported in the headset)
    const XrDuration period = s.predicted_period();
    const int fps = interp::output_fps();
    const double perFrame = period > 0 && fps > 0 ? std::clamp(1e9 / double(period) / double(fps), 1.0, 4.0) : 1.0;
    return s.predicted_display_time() + XrTime(double(frames) * perFrame * double(period));
}
// The head's views at a display time, shared by the game thread's VR work: the pad's input, the camera, the pad filter
// and the arms each located them, three to four xrLocateViews and xrLocateSpace calls a frame for the same prediction
// (docs/VR_PERFORMANCE.md, "One head pose a frame"). The camera's, which is what the headset shows, is always located
// fresh (`fresh`) and kept; the others reuse the latest for that time, so they also agree with the view. The render
// thread locates its own (vr_vulkan.cpp)
bool game_views(XrTime time, xr::Views& out, bool fresh = false) {
    static std::mutex mutex;
    static XrTime located = 0;
    static xr::Views views;
    static bool valid = false;
    std::lock_guard<std::mutex> lock(mutex);
    if (fresh || time != located || !valid) {
        located = time;
        valid = session().locate_views(time, views);
    }
    out = views;
    return valid;
}

// the anchor of the tracking space in the game: Link's position (blended like his model) at eye height
Vec3 anchor() {
    const float t = interp::pass_t();
    const bool blend = t < 1.0f && g_link.eyeBlendSeeded;
    const Vec3 p = t < 1.0f ? lerp(g_link.prevPos, g_link.pos, t) : g_link.pos;
    const Vec3 offset = blend ? lerp(g_link.prevEyeOffset, g_link.eyeOffset, t) : g_link.eyeOffset;
    const float height = blend ? g_link.prevEyeHeight + (g_link.eyeHeight - g_link.prevEyeHeight) * t : g_link.eyeHeight;
    return p + offset + Vec3{0, height, 0};
}

void recenter(const xr::Views& v) {
    // facing the way Link's model faces (shape_angle), read from him now (docs/VR_CONTROLS.md, "Facing after a door"):
    // g_link.heading is his frame's heading as of his last step (his travel direction in some actions), and after a
    // door the view came back facing the door he walked in by, his body then turned to it with the head (the game's
    // "Open" prompt up): reported in the headset, 2026-11-04
    float facing = g_link.heading;
    if (const uint32_t l = g_link.link) {
        const float model = s16_to_radians(int16_t(ld16(l + kShapeAngleY)));
        if (std::isfinite(model)) facing = model;
    }
    g_head.recenter(v.head, facing, g_link.link);
    g_body.set(yaw_of(v.head.orientation));
    g_anchorOwed = false;  // (the body may follow the head again: body_follows_head)
    LOG("[vr] recentred: heading %.1f degrees", double(g_map.heading * 180.0f / kPi));
}

// the head's pose in the game world for located views (game units): first-person play (HeadController::first_person)
Pose head_in_game(const xr::Views& v, const Config& c) {
    const float t = interp::pass_t();  // (the anchor's blend of Link's position, the origin's with it)
    // test captures only: WWHD_VR_TEST_PITCH=degrees looks down that much (the headset may lie level on a desk)
    static const float testPitch = [] { const char* e = std::getenv("WWHD_VR_TEST_PITCH"); return e ? float(std::atof(e)) : 0.0f; }();
    Pose head = v.head;
    if (testPitch != 0) head.orientation = normalize(head.orientation * axis_angle({1, 0, 0}, -testPitch * kPi / 180.0f));
    // (VR fork) leaning back: at most kLeanBackMetres behind his eyes along the way he faces, still inside his head (the
    // eyes are about 20 units in front of the head joint at its back). Farther, the view left his head behind and
    // showed his body from behind it, without its head (hidden in first person). Forward and to the sides as before.
    // Along his facing as drawn this frame (blended between the steps like his model and the anchor): the step's own
    // facing turned the limit in 30 Hz steps, which pushed a leaning-back view sideways in steps while he turned. The
    // walls hold the lean short of one in front (the last step's check: wall_lean_update)
    const float facing = blend_heading(g_link.prevHeading, g_link.heading, std::clamp(t, 0.0f, 1.0f));
    const float leanBack = g_link.valid && testPitch == 0 ? kLeanBackMetres * world_scale() : 0.0f;
    // and never under the ground he stands on (hold_above_floor: a tracking glitch put the view 1.25 m low); crawling,
    // his eyes are low, so at most half their height above his feet
    const Vec3 eyes = anchor();
    const float feet = t < 1.0f ? g_link.prevPos.y + (g_link.pos.y - g_link.prevPos.y) * t : g_link.pos.y;
    float floor = g_link.valid && std::isfinite(feet) && eyes.y > feet
                      ? feet + std::min(kViewAboveFeet, 0.5f * (eyes.y - feet)) : -INFINITY;
    // and, in the water, above its surface (Steam Frame video, 2026-11-03: swimming, the eyes sat at the surface, half
    // the view a flat sheet of water, and dipped under it into the black the game never draws below the sea)
    if (g_link.valid) floor = above_water(floor, feet, g_link.waterY);
    return g_head.first_person(head, eyes, facing, t, c.heightOffset, c.maxHeadOffset, world_scale(), leanBack, floor);
}
const char* stage_name() { return reinterpret_cast<const char*>(mem::ptr(play() + kPlayStage)); }

void sync_input_locked(const Config& c) {
    const auto now = std::chrono::steady_clock::now();
    // the pad is read several times per frame: sync the actions at most every 4 ms
    if (now - g_lastInput < std::chrono::milliseconds(4)) return;
    const float dt = std::chrono::duration<float>(now - g_lastInput).count();
    g_lastInput = now;
    xr::InputState in;
    // the motion detectors (swings, the off hand's swings, the leaf's flaps) measure between the poses' own times: the
    // pad is read up to every 4 ms, more than once a frame, while the controllers' poses are predicted for the frame's
    // display time and change once a frame. Measured by the clock, every second read saw no movement (0 m/s, which
    // undid the start's count) and the next a frame's movement over 4 ms (7 to 11 m/s): no thrust or downward swing
    // started in a play test, 2026-11-04. A read with the same pose time updates none of them
    const XrTime poseTime = display_time();
    static XrTime lastPoseTime = 0;
    const float poseDt = lastPoseTime && poseTime > lastPoseTime ? float(double(poseTime - lastPoseTime) * 1e-9) : 0.0f;
    const bool newPose = poseTime != lastPoseTime;
    lastPoseTime = poseTime;
    if (!session().sync_input(poseTime, c.leftHanded, in)) {
        g_input = {};
        g_gripLatch[0] = g_gripLatch[1] = GripLatch{};  // no input: the hands open (no stuck fist)
        return;
    }
    // each controller's grip closes its own Link hand (physical sides; handedness only decides the sword hand)
    const int swordSide = dominant_side(c);
    const bool swordGripWas = g_gripLatch[swordSide].closed;
    for (int h = 0; h < 2; ++h) {
        if (!in.hand[h].active) g_gripLatch[h] = GripLatch{};
        else g_gripLatch[h].update(in.hand[h].grip_value);
        g_gripMax[h] = std::max(g_gripMax[h], in.hand[h].grip_value);
    }
    // tracking diagnostics: reads a hand was held through a gap, and losses (every 10 s with the input summary)
    for (int h = 0; h < 2; ++h) {
        g_trackHeld[h] += in.hand[h].held;
        g_estimatedReads[h] += in.hand[h].estimated;
        g_trackLost[h] += g_input.hand[h].active && !in.hand[h].active;
    }
    g_input = in;
    // the controllers' pointing ray (aim pose) in their grip frame (fixed per controller and runtime): its pitch above
    // the line through the fist and its yaw, logged once per hand (docs/VR_HELD_ITEMS.md, the blade's direction)
    {
        static bool logged[2] = {};
        for (int h = 0; h < 2; ++h) {
            if (logged[h] || !in.hand[h].active) continue;
            const Vec3 aimInGrip = rotate(conjugate(in.hand[h].grip.orientation), rotate(in.hand[h].aim.orientation, {0, 0, -1}));
            const Vec3 offset = rotate(conjugate(in.hand[h].grip.orientation), in.hand[h].aim.position - in.hand[h].grip.position);
            if (!finite(aimInGrip)) continue;
            logged[h] = true;
            LOG("[vr] %s controller: its pointing ray is %.1f degrees above the line through the fist (yaw %.1f), "
                "starting %.3f, %.3f, %.3f m from the grip", h ? "right" : "left",
                double(std::atan2(aimInGrip.y, -aimInGrip.z) * 180.0f / kPi),
                double(std::atan2(aimInGrip.x, -aimInGrip.z) * 180.0f / kPi), double(offset.x), double(offset.y),
                double(offset.z));
        }
    }
    // the shoulder draw: the sword hand over its shoulder (behind the head, in the body's frame) and its grip pressed
    // (the press, not a held grip) while the sword is sheathed and nothing else is in the hand
    {
        xr::Views hv;
        const bool haveHead = in.hand[swordSide].active && game_views(display_time(), hv);
        bool reach = false;
        if (haveHead) {
            const Vec3 rel = in.hand[swordSide].grip.position - hv.head.position;
            reach = g_reach.update(rotate(conjugate(yaw_quat(g_bodyYaw)), rel), swordSide ? 1 : -1);
        } else {
            g_reach.inside = false;
        }
        const bool press = !swordGripWas && g_gripLatch[swordSide].closed;
        if (press && reach && g_sword.state == SwordState::Sheathed && g_held.gameHand < 0 && sword_gates() &&
            !g_wheel.consumes_stick() && !conducting()) {  // (conducting: the hands play the Wind Waker)
            g_drawRequested = true;
            const uint64_t st = interp::logic_steps();
            g_bFrom = st;
            g_bTo = st + 2;  // the game's own draw (B), its slash kept from hitting (link_executed)
            g_bHammer = g_bThrust = g_bParry = g_bVertical = g_bJump = g_bComboReset = false;
            LOG("[vr] sword: drawn from over the %s shoulder", swordSide ? "right" : "left");
        }
    }
    // swing detection: the sword hand's blade in the tracking space
    const int mainHand = dominant_side(c);
    if (in.hand[mainHand].active && !g_thirdPerson.load()) {  // third person: no motion controls (swings)
        g_swing.configure(c);
        g_swing.set_locked_on(locked_on(g_link.link && g_link.valid ? g_link.link : 0) && first_person_play());
        const SwingEvent e = newPose ? g_swing.update(blade_from_grip(sword_base(in.hand[mainHand]), c), poseDt, g_bodyYaw)
                                     : SwingEvent{};
        if (e.started) g_ledger.begin_swing();
        // (diagnostics, play tests: thrusts and chops that never started a swing) once a second at most, the sword hand's
        // fastest movement while no swing ran, when it came within half of the start speed but not to it, and where
        // it went (body frame)
        {
            static float peak = 0;
            static Vec3 peakV;
            static bool started = false;
            static auto last = std::chrono::steady_clock::now();
            started |= e.started;
            if (g_swing.phase() == SwingPhase::Idle && g_held.sword && e.speed > peak) {
                peak = e.speed;
                peakV = rotate(yaw_quat(-g_bodyYaw), g_swing.tip_velocity());
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - last >= std::chrono::seconds(1)) {
                if (!started && peak >= 0.5f * g_swing.start_speed())
                    LOG("[vr] swing: none started; the sword hand's fastest was %.2f m/s (start %.2f; thrusts %.2f): "
                        "%.2f across, %.2f up, %.2f forward", double(peak), double(g_swing.start_speed()),
                        double(g_swing.start_speed() * 0.6f), double(peakV.x / std::max(peak, 1e-3f)),
                        double(peakV.y / std::max(peak, 1e-3f)), double(-peakV.z / std::max(peak, 1e-3f)));
                peak = 0;
                started = false;
                last = now;
            }
        }
        if (e.started) {  // (the jump attack: a downward swing begun with the hand above the head)
            xr::Views sv;
            // (kJumpAttackAbove over the head: at head height, an ordinary overhead chop was taken for the jump attack
            // and the vertical slash Orca asked for never came; training test 2, 2026-11-04)
            g_swingHigh = game_views(display_time(), sv) &&
                          in.hand[mainHand].grip.position.y > sv.head.position.y + kJumpAttackAbove;
        }
        if (e.ended)
            LOG("[vr] swing: ended at step %llu: the blade turned %.0f degrees, the tip moved %.2f m across, %.2f m up, "
                "%.2f m forward%s", (unsigned long long)interp::logic_steps(), double(e.turned * 180.0f / kPi),
                double(e.moved.x), double(e.moved.y), double(-e.moved.z), e.wasFull ? " (full: attacked)" : " (no attack)");
        // the game's attack (its animation and sound) only for a full swing: down, left to right or right to left
        // (vr.swingFullDegrees; asked for in the headset: a swing's start, a thrust or a flick played them too).
        // B held for two logic steps, so the press lands on a logic step the game reads; only with the sword in the
        // hand (a swing with the sword sheathed neither draws it nor attacks)
        // A thrust (forward along the blade) is the game's forward thrust: B with the stick forward (thrust_press); not
        // with the hammer, which has none
        // (a parry's finishing cut, parry_cut_wait: from the swing's start, the jump's window from its height to the
        // landing being short; second player test, 2026-11-04)
        if (e.started || e.full) g_parry.lastSwing = interp::logic_steps();
        if (e.full) g_parry.lastFull = interp::logic_steps();
        // (easier, asked for after a play test, 2026-11-05: the parry from any swing's start in its window, any
        // direction, not only once it is a full one; A for three steps)
        // (and a flick: the sword hand at kParryFlick of the start speed in the window, no swing needed; 2026-11-05)
        constexpr float kParryFlick = 0.4f;
        if (kParryBySwing && !e.started && g_held.sword && e.speed >= kParryFlick * g_swing.start_speed() && g_parry.ready &&
            !g_held.hammer && !g_parry.inMove && !(g_bParry && interp::logic_steps() < g_bTo) &&
            aspect::gameplay_camera() && g_sword.can_swing()) {
            const uint64_t s = interp::logic_steps();
            g_bFrom = s;
            g_bTo = s + 3;
            g_bParry = true;
            g_bThrust = g_bVertical = g_bHammer = g_bJump = g_bComboReset = false;
            g_parry.lastSwing = s;
            LOG("[vr] parry: a flick of the sword hand (%.2f m/s) in the parry's window at step %llu: A", double(e.speed),
                (unsigned long long)s);
        }
        if (kParryBySwing && e.started && g_parry.ready && !g_held.hammer && !g_parry.inMove &&
            !(g_bParry && interp::logic_steps() < g_bTo) && aspect::gameplay_camera() && g_sword.can_swing()) {
            const uint64_t s = interp::logic_steps();
            g_bFrom = s;
            g_bTo = s + 3;
            g_bParry = true;
            g_bThrust = g_bVertical = g_bHammer = g_bJump = g_bComboReset = false;
            LOG("[vr] parry: a swing began in the parry's window at step %llu: A", (unsigned long long)s);
        }
        if (e.full && !(e.fullKind == SwingKind::Thrust && g_held.hammer)) {
            const uint64_t s = interp::logic_steps();
            // (in a parry's move the swing finishes it, parry_cut_wait: no press of its own)
            // (not while a parry's A from the swing's start is being pressed: kept)
            const uint64_t sinceLast = g_lastFullSwing && s >= g_lastFullSwing ? s - g_lastFullSwing : ~uint64_t{0};
            const bool phantom = sinceLast <= kPhantomSteps && !g_parry.ready && !g_held.hammer;
            g_lastFullSwing = s;
            if (phantom)
                LOG("[vr] swing: a full %s at step %llu ignored: the hand coming back, %llu steps after the last",
                    e.fullKind == SwingKind::Thrust ? "thrust" : e.fullKind == SwingKind::Vertical ? "downward swing"
                                                                                                 : "side swing",
                    (unsigned long long)s, (unsigned long long)sinceLast);
            if (!phantom && aspect::gameplay_camera() && g_sword.can_swing() && !g_parry.inMove && !(g_bParry && s < g_bTo)) {
                g_bFrom = s;
                g_bTo = s + 2;
                g_bThrust = e.fullKind == SwingKind::Thrust;
                // (the vertical slash only locked on, as the game: daPy_lk_c::changeCutProc gives it only then, B with
                // the stick in the middle; not locked on a downward swing is the plain slash, B as it is; asked for,
                // 2026-11-04: "ensure vertical slicing requires lock on")
                g_bVertical = e.fullKind == SwingKind::Vertical && g_link.link && g_link.valid && locked_on(g_link.link) &&
                              !g_held.hammer && first_person_play();
                g_bSide = e.fullSide;
                g_bParry = kParryBySwing && g_parry.ready && !g_held.hammer;  // (A: the game's parry; docs/VR_CONTROLS.md)
                if (g_bParry) LOG("[vr] parry: a swing in the parry's window at step %llu: A", (unsigned long long)s);
                // locked on (docs/VR_CONTROLS.md, "Locked-on sword moves")
                const uint32_t lk = g_link.link && g_link.valid ? g_link.link : 0;
                const bool locked = locked_on(lk) && !g_held.hammer && first_person_play();
                const uint32_t pl = play();
                g_bJump = !g_bParry && locked && g_bVertical && g_swingHigh && pl &&
                          ld8(pl + kPlayDoStatus) == kDoStatusJumpAttack;
                // (a thrust starts the combo again locked on or not, so the 4th is a thrust too, not the finisher)
                g_bComboReset = !g_bParry && !g_bJump && !g_held.hammer && first_person_play() &&
                                (g_bThrust || (locked && g_bVertical));
                if (g_bJump) LOG("[vr] sword: a swing from above the head, locked on: the jump attack (A)");
                if (kParryBySwing && !g_bParry && locked && parry_window_near(lk)) {
                    g_bFrom = g_bTo = 0;  // (no press: held for the parry, kParryBuffer)
                    g_bJump = g_bComboReset = false;
                    LOG("[vr] parry: a swing just before the window opens at step %llu: held for the parry", (unsigned long long)s);
                }
                g_bHammer = g_held.hammer;  // the swing's weapon, kept for its press (never B for the hammer)
                // (queued: pressed again if it starts no sword move, swing_queue_step; not a parry, jump or the hammer)
                if (g_bTo && !g_bParry && !g_bJump && !g_held.hammer)
                    g_queued = {true, s + kQueueSteps, g_moveSerial, g_bThrust, g_bVertical, g_bComboReset, g_bSide};
                else
                    g_queued.active = false;
                if (g_held.hammer) {
                    g_hammerStick = hammer_swing_stick(e.fullKind == SwingKind::Horizontal, e.fullSide);
                    g_hammerPressStep = s;
                    g_hammerSkipped = false;
                }
            }
            LOG("[vr] swing: a full %s at step %llu%s: the hand %.2f m across, %.2f m up, %.2f m forward, the blade %.0f "
                "degrees up, turned %.0f since the start", e.fullKind == SwingKind::Thrust ? "thrust" : e.fullKind == SwingKind::Vertical ? "downward swing" : e.fullSide > 0 ? "swing to the right" : "swing to the left",
                (unsigned long long)s,
                g_link.link && g_link.valid && locked_on(g_link.link) ? " (locked on)" : " (not locked on)",
                double(e.fullPath.x), double(e.fullPath.y), double(-e.fullPath.z),
                double(std::asin(std::clamp(e.fullBladeUp, -1.0f, 1.0f)) * 180.0f / kPi),
                double(std::acos(std::clamp(e.fullBladeTurnCos, -1.0f, 1.0f)) * 180.0f / kPi));
        }
    } else {
        g_swing.reset();
    }
    // the boomerang (in the off hand, vr_sword.h off_hand_item): a full downward swing of that hand throws it, the
    // sword's swing detection run on it (its thresholds, its full-swing rule; asked for in the headset): its button
    // pressed for two logic steps and let go (the game throws on the release, checkNextActionBoomerangReady)
    {
        const int off = 0;  // the left hand either way (vr_sword.h fixed_item_side)
        const uint32_t l = g_link.link;
        // (aimed or only held: caught again, Link holds it without aiming)
        const bool boomerang = l && g_link.valid && ld16(l + 0x69B0) == kItemBoomerang;
        if (boomerang && in.hand[off].active && !g_thirdPerson.load() && aspect::gameplay_camera()) {
            g_offSwing.configure(c);
            const SwingEvent e = newPose ? g_offSwing.update(blade_from_grip(sword_base(in.hand[off]), c), poseDt, g_bodyYaw)
                                         : SwingEvent{};
            if (e.full && e.fullKind == SwingKind::Vertical) {
                const uint64_t st = interp::logic_steps();
                g_throwFrom = st;
                g_throwTo = st + 2;
                LOG("[vr] boomerang: thrown by a downward swing of the off hand at step %llu", (unsigned long long)st);
            }
        } else {
            g_offSwing.reset();
        }
        // the Deku Leaf (in the left hand either way, vr_sword.h fixed_item_side): a quick swing of that hand up or
        // down presses its button for two logic steps (asked for in the headset: a fan swing on the ground, the glide
        // opened in the air), as the left trigger does (filter_pad)
        const bool leaf = l && g_link.valid && ld16(l + 0x69B0) == kItemDekuLeaf;
        if (leaf && in.hand[off].active && !g_thirdPerson.load() && aspect::gameplay_camera()) {
            g_leafFlap.configure(c);
            if (const int f = newPose ? g_leafFlap.update(blade_from_grip(sword_base(in.hand[off]), c).tip, poseDt) : 0) {
                const uint64_t st = interp::logic_steps();
                g_throwFrom = st;
                g_throwTo = st + 2;
                LOG("[vr] Deku Leaf: used by a swing of the left hand %s at step %llu", f > 0 ? "up" : "down",
                    (unsigned long long)st);
            }
        } else {
            g_leafFlap.reset();
        }
    }
}
}  // namespace

void request_recenter() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_head.request_recenter();
}
void take_camera_ahead(uint64_t (&ahead)[5], uint64_t& changes) {
    for (int i = 0; i < 5; ++i) ahead[i] = g_cameraAhead[i].exchange(0, std::memory_order_relaxed);
    changes = g_cameraAheadChanges.exchange(0, std::memory_order_relaxed);
}

// ---- camera
bool screen_blur_off() { return running() && !config().screenBlur; }
bool depth_of_field_off() { return running() && !config().depthOfField; }
bool blur_head_turn(float a[6]) {
    Fov f;
    if (!running() || !projection(f)) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_head.blur_reprojection(f, a);
}

// ---- the boat (docs/VR_SAILING_VIEWS.md): the King of Red Lions (daShip_c), play's ship pointer (mpPlayerPtr[2]).
// Read only while Link rides it (player status 0 SHIP_RIDE): then it exists. Its frame is its actor's position and
// shape_angle (logic only, never blended in place), sampled once a logic step and blended as its model is drawn; its
// geometry (the mast, the figurehead's head, Link's seat) from its models in its own frame, until it holds still
constexpr uint32_t kPlayShip = 0x5B3C, kShipRide = 0x10000;
constexpr uint32_t kShipShapeAngle = 0x328;                    // fopAc_ac_c::shape_angle (x, y, z)
constexpr uint32_t kShipBodyAnm = 0x3B4, kShipHeadAnm = 0x3B8;  // mpBodyAnm, mpHeadAnm (mDoExt_McaMorf)
constexpr uint32_t kMorfModel = 0x90;                          // mDoExt_McaMorf::mpModel
constexpr uint32_t kShipCurMode = 0x635;                       // daShip_c::mCurMode
constexpr int kShipMastJoint = 6, kShipHeadJoint = 8;          // FN_BODY J_FN_MAST, FN_HEAD_H J_FN_ATAMA
BoatFrames g_boat;
BoatMeasure g_boatMeasure;
// the Free Roam Camera (vr_sail_view.h): its zoom and slide, reset each time the view begins; moved each frame by the
// triggers and the right stick (CameraOverride), over the time between the frames' display times
FreeRoamCamera g_freeRoam;
XrTime g_freeRoamShown = 0;
uint32_t riding_ship() {
    return (ld32(play() + kPlayPlayerStatus0) & kShipRide) ? ld32(play() + kPlayShip) : 0;
}
SailState sail_state() {
    SailState s;
    const uint32_t ship = riding_ship();
    s.riding = (ld32(play() + kPlayPlayerStatus0) & kShipRide) != 0;
    s.ship = ship && finite(ld_vec(ship + kPos));
    s.shipMode = s.ship ? ld8(ship + kShipCurMode) : 0xFF;
    s.greatSea = !std::strcmp(stage_name(), "sea");
    s.playCamera = aspect::gameplay_camera();
    s.menu = gfx::display_gamepad_menu_open();
    s.zoomView = subject_zoom_view();
    return s;
}
// a joint's position (world) from a model's joint matrices
bool model_joint_pos(uint32_t model, int joint, Vec3& out) {
    const uint32_t block = model ? ld32(model + kMtxBlock) : 0;
    const uint32_t mtx = block ? ld32(block + kMtxBlockMatrices) : 0;
    if (!mtx || ld16(block + 0x2C) <= joint) return false;
    out = {float(ldf32(mtx + joint * 48 + 12)), float(ldf32(mtx + joint * 48 + 28)), float(ldf32(mtx + joint * 48 + 44))};
    return finite(out);
}
// the mast, the figurehead's head and Link in the boat's own frame: from its body model's base matrix (the frame the
// joints were calculated in, so the two always agree)
bool measure_boat(uint32_t ship, Vec3 linkPos, BoatGeometry& g) {
    const uint32_t bodyAnm = ld32(ship + kShipBodyAnm), headAnm = ld32(ship + kShipHeadAnm);
    const uint32_t body = bodyAnm ? ld32(bodyAnm + kMorfModel) : 0, head = headAnm ? ld32(headAnm + kMorfModel) : 0;
    if (!body || !head) return false;
    float m[12];
    for (int i = 0; i < 12; ++i) m[i] = float(ldf32(body + kModelBaseMtx + i * 4));
    const Vec3 x{m[0], m[4], m[8]}, y{m[1], m[5], m[9]}, z{m[2], m[6], m[10]}, at{m[3], m[7], m[11]};
    auto local = [&](Vec3 w) { const Vec3 d = w - at; return Vec3{dot(x, d), dot(y, d), dot(z, d)}; };
    Vec3 mast, top;
    if (!finite(at) || !model_joint_pos(body, kShipMastJoint, mast) || !model_joint_pos(head, kShipHeadJoint, top) ||
        !finite(linkPos))
        return false;
    g.mast = local(mast);
    g.head = local(top);
    g.seat = local(linkPos);
    return true;
}
// each frame: the boat's frame at a new logic step, its geometry until measured
void update_boat() {
    const uint32_t ship = riding_ship();
    if (!ship) {
        g_boat.reset();
        return;
    }
    const Vec3 pos = ld_vec(ship + kPos);
    if (!finite(pos)) return;
    const uint64_t step = interp::logic_steps();
    if (g_boat.valid && g_boat.ship == ship && g_boat.step == step) return;
    g_boat.sample(ship, step, {pos, int16_t(ld16(ship + kShipShapeAngle)), int16_t(ld16(ship + kShipShapeAngle + 2)),
                               int16_t(ld16(ship + kShipShapeAngle + 4))});
    BoatGeometry g;
    if (g_boatMeasure.kept.measured && g_boatMeasure.ship == ship) return;
    if (!g_link.valid || !measure_boat(ship, g_link.pos, g)) return;
    if (g_boatMeasure.add(ship, g))
        LOG("[vr] boat: measured in its own frame: the mast at %.0f,%.0f,%.0f, the figurehead's head at %.0f,%.0f,%.0f, "
            "Link at %.0f,%.0f,%.0f (units: +x left, +y up, +z the bow)", double(g.mast.x), double(g.mast.y),
            double(g.mast.z), double(g.head.x), double(g.head.y), double(g.head.z), double(g.seat.x), double(g.seat.y),
            double(g.seat.z));
}
// the boat's views end as soon as sailing does (a cutscene, a talk, a menu, getting off, another stage...): third
// person, the game's camera. Called each frame and at each pad read
void end_boat_view_unless_sailing() {
    if (!boat_view_mode(g_view.mode)) return;
    const SailState s = sail_state();
    if (!g_view.update(sailing_eligible(s))) return;
    g_head.end_game_camera();
    char why[160];
    std::snprintf(why, sizeof why, "(the boat's views end: %s)", sail_blocker(s));
    view_mode_changed(why);
}

// A conversation's text held in the world in first person (docs/VR_CUTSCENE_TEXT.md, "Conversations in first person";
// asked for in the headset). The game draws it as a flat layout at the centre of the picture, so it followed the head.
// Its direction is taken where the view looks when the conversation begins (game space, so the stick's and the
// view's turns are in it too); each frame the layouts are moved across and up (layout_offset_x / _y, in the picture's
// half widths) to where that direction is in the view now, as a sign at a great distance would be (they are drawn
// alike in both eyes). vr.textHeight still moves it from there. Looking far away moves it out of view
// With the eyes' own projections (the game's UI in its own layer) the layouts stay centred in the UI image and the UI
// layer's quad itself is held in that direction (docs/VR_CUTSCENE_TEXT.md, "Dialogue in the UI layer"): the direction is
// handed over in the headset's tracking space (the game-to-headset turn of this pass: the stick's turns stay in it), and
// the compositor holds the quad there at the display's own head pose. The layouts' offsets had moved the text in the
// picture once a game pass, with the layouts' clip moved apart from it each draw: through Virtual Desktop the text
// showed cut off, then not at all while its box's arrow stayed
struct TalkText { bool active = false; Vec3 dir; } g_talkText;
void talk_text_anchor(const Pose& head, const Quat& headTracking, const Config& c) {
    if (!talk_first_person()) {
        if (g_talkText.active) LOG("[vr] conversation in first person: ended");
        g_talkText.active = false;
        set_talk_text_dir(false);
        return;
    }
    const Vec3 forward = rotate(head.orientation, Vec3{0, 0, -1});
    if (!g_talkText.active) {
        if (!finite(forward)) return;
        g_talkText = {true, normalize(forward)};
        LOG("[vr] conversation in first person: the view stays at Link's eyes, the text held where you look");
    }
    if (own_eye_projection()) {
        set_talk_text_dir(true, rotate(headTracking * conjugate(head.orientation), g_talkText.dir));
        set_layout_offset_x(0.0f);
        set_layout_offset_y(c.textHeight);
        return;
    }
    Fov f;
    if (!projection(f)) f = Fov{};
    const Vec3 local = rotate(conjugate(head.orientation), g_talkText.dir);  // (x right, y up, -z ahead)
    float x = 4.0f, y = 0.0f;  // (behind you: out of view)
    if (local.z < -0.05f) {
        const float tx = local.x / -local.z, ty = local.y / -local.z;
        const float l = std::tan(f.left), r = std::tan(f.right), u = std::tan(f.up), d = std::tan(f.down);
        x = (2.0f * tx - (r + l)) / (r - l);
        y = (2.0f * ty - (u + d)) / (u - d);
    }
    set_layout_offset_x(x);
    set_layout_offset_y(y + c.textHeight);
}

CameraOverride::CameraOverride(uint32_t cam) {
    GameWorkTimer workTimer(GameWork::Camera);  // (docs/VR_PERFORMANCE.md: the VR work on the game thread)
    if (!running() || !cam) return;
    xr::Views v;
    int ahead = 0;
    // the runtime moved the tracking space's origin (a recentre on the headset): recentre the game on it too
    if (session().take_space_changed()) request_recenter();
    if (!game_views(display_time(&ahead), v, true)) return;
    {
        static int previous = -1;
        g_cameraAhead[ahead].fetch_add(1, std::memory_order_relaxed);
        if (previous >= 0 && ahead != previous) g_cameraAheadChanges.fetch_add(1, std::memory_order_relaxed);
        previous = ahead;
    }
    const Config c = config();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_head.begin_frame(int64_t(v.time));  // (a new frame: its view is set below, when it is drawn from the head)
    // the game camera's heading as its own logic left it (the left stick is relative to it)
    const Vec3 eye = ld_vec(cam + kCamEye), center = ld_vec(cam + kCamCenter);
    if (finite(eye) && finite(center) && length(center - eye) > 1e-3f) {
        g_cameraHeading = game_heading(center - eye);
        g_haveCameraHeading = true;
    }
    // the view at the session's start: vr.startThirdPerson (once; the right stick click toggles it from then on)
    static bool viewChosen = false;
    if (!viewChosen) {
        viewChosen = true;
        if (c.startThirdPerson && !g_thirdPerson.load()) {
            g_view.mode = ViewMode::ThirdPerson;
            view_mode_changed("at the start (vr.startThirdPerson)");
        }
    }
    // the boat (its frame each step) and the boat's views, which end as soon as sailing does
    update_boat();
    end_boat_view_unless_sailing();
    float eyeX[2];
    const xr::Views rendered = as_rendered(v, eyeX);
    const bool gameplay = play_view() && g_link.valid;
    Pose head;
    g_passHeadValid = false;
    ++g_passSerial;
    // smooth turning: this frame's share, over the time since the frame before (g_smoothTurn), before the view is built
    {
        const auto now = std::chrono::steady_clock::now();
        float dt = g_smoothTurn.lastFrame.time_since_epoch().count()
                       ? std::chrono::duration<float>(now - g_smoothTurn.lastFrame).count() : 0.0f;
        g_smoothTurn.lastFrame = now;
        // the frames' predicted display times (as the head's pose is predicted for): the time between them as shown.
        // The time these views were located for, not display_time() again: the render thread may have predicted
        // anew in between, and the turn then went by another interval than the head's pose
        const XrTime shown = v.time;
        if (g_smoothTurn.lastDisplay > 0 && shown >= g_smoothTurn.lastDisplay) {
            const float d = float(double(shown - g_smoothTurn.lastDisplay) * 1e-9);
            if (d < 0.1f) dt = d;  // (0: a second pass for the same frame turns nothing more)
        }
        g_smoothTurn.lastDisplay = shown;
        const bool fresh = std::chrono::duration<float>(now - g_smoothTurn.read).count() < kSmoothTurnStale;
        // the controllers of this frame (the stick for the smooth turn, the hands' poses for the arms below): read only
        // at the game's 30 Hz pad reads, the arms moved in 30 Hz steps in the headset (Quest 3 report: "arm movement
        // fps looks off"; 300 input syncs in 10 s in play against 900 in menus). Every frame while the pad is read in
        // first person (sync_input_locked reads at most every 4 ms)
        const bool padLive = std::chrono::duration<float>(now - g_padReadAt).count() < kSmoothTurnStale;
        // (the Free Roam Camera too: its triggers and stick move it every frame)
        if ((c.turn == Turn::Smooth && fresh && g_smoothTurn.allowed) ||
            (padLive && (!g_thirdPerson.load() || g_view.mode == ViewMode::FreeRoam)))
            sync_input_locked(c);
        const float stickX = g_input.valid ? g_input.hand[1].stickX : 0.0f;  // (the right controller's, as the pad read)
        if (c.turn == Turn::Smooth && fresh && g_smoothTurn.allowed && stickX != 0) {
            const float turn = g_turner.update(stickX, dt, c);
            if (g_view.mode == ViewMode::Neck)
                g_head.boat_turn(turn);  // (the neck view: the view turns about its anchor, Link not)
            else
                g_head.turn(turn);  // (frame_correct: Link's body turns with it every pass)
            // (log, every 10 s of turning) the frames turned and their steps: even when each frame turns its share
            static int frames = 0;
            static double sum = 0, most = 0;
            static auto since = now;
            const double deg = std::fabs(turn) * 180.0 / kPi;
            ++frames;
            sum += deg;
            most = std::max(most, deg);
            if (now - since > std::chrono::seconds(10)) {
                LOG("[vr] smooth turn: %d frames turned in 10 s, %.2f degrees a frame on average, %.2f at most", frames,
                    sum / frames, most);
                frames = 0;
                sum = most = 0;
                since = now;
            }
        }
        // a ladder, a ledge or a lock-on: the view's base turns to Link by this frame's share (HeadController's rig
        // turn); not while the pad is not read (a pause), as the smooth turn
        if (gameplay && !g_thirdPerson.load() && padLive && g_head.advance_rig_turn(dt) && g_head.rig_turn().lockOn)
            g_body.set(body_yaw_of(g_link.heading, g_map.heading));
    }
    // third person: conducting begun or ended, the head's forward is taken again, so looking ahead shows the game's
    // own camera for it (the Wind Waker's, in front of Link), as an event's does
    static bool wasConducting = false;
    const bool conductingNow = conducting();
    const bool conductingChanged = conductingNow != wasConducting;
    wasConducting = conductingNow;
    if (gameplay && g_view.mode == ViewMode::FreeRoam && g_boat.valid) {
        // the Free Roam Camera (docs/VR_SAILING_VIEWS.md): the tracking space above and behind the boat, moved by the
        // triggers (zoom: left in, right out) and the right stick (slide), following the boat's position and turn as
        // drawn but not its pitch or roll; facing the bow; the head's own pose added in full. Each time it begins it
        // starts again from its first place
        const float t = std::clamp(interp::pass_t(), 0.0f, 1.0f);
        if (g_head.boat_view() != int(ViewMode::FreeRoam)) {
            g_freeRoam.reset();
            g_freeRoamShown = v.time;
            g_head.begin_boat_view(v.head, int(ViewMode::FreeRoam));
            const Vec3 at = g_freeRoam.local(g_boatMeasure.geometry());
            LOG("[vr] free roam: begins at %.0f,%.0f,%.0f in the boat's frame (%s geometry), %.0f units from its middle",
                double(at.x), double(at.y), double(at.z), g_boatMeasure.kept.measured ? "its measured" : "the default",
                double(g_freeRoam.distance));
        }
        // this frame's share of the triggers and the stick: the time between the frames' predicted display times (a
        // second pass of the same frame moves nothing); not while the pad is not read (a pause)
        const bool padLive = std::chrono::duration<float>(std::chrono::steady_clock::now() - g_padReadAt).count() <
                             kSmoothTurnStale;
        if (v.time > g_freeRoamShown) {
            const float dt = std::min(float(double(v.time - g_freeRoamShown) * 1e-9), 0.1f);
            g_freeRoamShown = v.time;
            if (padLive && g_input.valid) {
                // (the right stick aims an item meanwhile: no slide, g_boatAimStick)
                const Stick slide = g_boatAimStick ? Stick{}
                                                   : dead_zone({g_input.hand[1].stickX, g_input.hand[1].stickY}, c.stickDeadZone);
                const float before = g_freeRoam.distance;
                g_freeRoam.update(g_input.hand[0].trigger, g_input.hand[1].trigger, slide.x, slide.y, dt);
                // (log) the zoom's ends, once each time one is reached
                const FreeRoamOffsets& o = kFreeRoam;
                if (before != g_freeRoam.distance &&
                    (g_freeRoam.distance == o.minDistance || g_freeRoam.distance == o.maxDistance))
                    LOG("[vr] free roam: zoomed %s (%.0f units)", g_freeRoam.distance == o.minDistance ? "in as far as it goes"
                                                                                                      : "out as far as it goes",
                        double(g_freeRoam.distance));
            }
        }
        const Vec3 at = boat_level_to_world(g_boat, g_freeRoam.local(g_boatMeasure.geometry()), t);
        const float heading = wrap_radians(g_boat.heading(t));
        if (!finite(at) || !std::isfinite(heading)) return;
        head = g_head.boat_view_pose(v.head, at, heading, c.maxHeadOffset, world_scale());
    } else if (gameplay && boat_view_mode(g_view.mode) && g_boat.valid) {
        // one of the boat's views (docs/VR_SAILING_VIEWS.md): the tracking space at its anchor on the boat, blended
        // with the boat as drawn, facing the boat's heading plus the view's; the head's own pose added in full
        const float t = std::clamp(interp::pass_t(), 0.0f, 1.0f);
        const SailAnchor a = sail_anchor(g_view.mode, g_boatMeasure.geometry());
        const Vec3 at = g_boat.to_world(a.local, t);
        const float heading = wrap_radians(g_boat.heading(t) + a.yaw);
        if (g_head.boat_view() != int(g_view.mode)) {
            g_head.begin_boat_view(v.head, int(g_view.mode));
            LOG("[vr] boat view: anchored at %.0f,%.0f,%.0f in the boat's frame (%s geometry), facing %.0f degrees from "
                "its bow", double(a.local.x), double(a.local.y), double(a.local.z),
                g_boatMeasure.kept.measured ? "its measured" : "the default", double(a.yaw * 180.0f / kPi));
        }
        if (!finite(at) || !std::isfinite(heading)) return;
        head = g_head.boat_view_pose(v.head, at, heading, c.maxHeadOffset, world_scale());
    } else if (gameplay && g_thirdPerson.load()) {
        // third person: the game's own camera (its follow, framing and collision), turned by the head relative to
        // where it looked when the view was switched (as in events); the head's position does not move it
        if (!g_head.game_camera() || conductingChanged) g_head.begin_game_camera(yaw_of(v.head.orientation));
        const Vec3 up = ld_vec(cam + kCamUp);
        if (!finite(eye) || !finite(center) || !finite(up) || length(center - eye) < 1e-3f) return;
        head = g_head.game_camera_view(eye, center, up, v.head.orientation);
    } else if (gameplay) {
        // (a re-anchor waiting for Link to be in your hands after a door, an event or a new stage: below)
        static uint64_t pendingSince = 0;
        if (g_head.end_game_camera()) {
            // first person again after an event, a cutscene or third person (docs/VR_CONTROLS.md, "Tracking"): the
            // event may have moved and turned Link, and you may have walked or turned in the room meanwhile. The view
            // is anchored again at your head, facing his way, as TPVR calibrates at every first-person activation;
            // with the mapping left as it was, his arms reached for controllers away from his body and the hands no
            // longer matched them until a recentre. The camera cuts here anyway, so nothing jumps in view.
            if (g_head.calibrated_for(g_link.link)) {
                recenter(v);
                ++g_reanchors;
                LOG("[vr] first person again: the view anchored at your head, facing Link's way");
                // (a door within a stage is an event: anchored once more when he is in your hands, below)
                pendingSince = std::max<uint64_t>(interp::logic_steps(), 1);
            }
        }
        // a new stage (a door, a cave, a load): anchored again facing Link's model on its first first-person frame,
        // whether or not an event came between (docs/VR_CONTROLS.md, "Facing after a door")
        {
            // (and again once he is first in your hands there, standing or walking in normal play, the door's or the
            // entry's walk over: on the first frame he could still stand as the door left him, facing it, and the view
            // and his body faced the door; asked for, 2026-11-05: "he should enter facing forward towards the open room
            // not back facing the door")
            static char lastStage[9] = {};
            const char* st = stage_name();
            const uint64_t nowStep = interp::logic_steps();
            if (std::strncmp(st, lastStage, 8) != 0) {
                const bool had = lastStage[0] != 0;
                std::strncpy(lastStage, st, 8);
                if (had && g_head.calibrated_for(g_link.link)) {
                    recenter(v);
                    ++g_reanchors;
                    LOG("[vr] new stage %s: the view anchored at your head, facing Link's way", lastStage);
                }
                if (had) pendingSince = nowStep ? nowStep : 1;
            }
            if (pendingSince) {
                const uint32_t l = g_link.link;
                const uint32_t pr = l && g_link.valid ? ld32(l + kCurProc) : 0;
                if (nowStep > pendingSince + 300) {
                    pendingSince = 0;  // (10 s: never in your hands; left as it is)
                } else if (play_view() && (pr == 4 || pr == 6) && g_head.calibrated_for(l)) {
                    pendingSince = 0;
                    recenter(v);
                    ++g_reanchors;
                    LOG("[vr] in your hands (action 0x%X, stage %s): the view and body face his way",
                        pr, lastStage);
                }
            }
        }
        if (!g_head.calibrated_for(g_link.link)) recenter(v);
        {
            // (diagnostics, the input summary) how far the head is from where the view is anchored, horizontally
            const Vec3 d = v.head.position - g_map.origin;
            g_maxLean = std::max(g_maxLean, std::sqrt(d.x * d.x + d.z * d.z));
        }
        head = head_in_game(v, c);
        g_passHead = head;
        g_passHeadValid = true;
        talk_text_anchor(head, v.head.orientation, c);
        {
            // the hands every frame (frame_correct): each controller's wrist target now (this pass's eyes plus its
            // offset, as link_arms aims) against the blend of the two steps' targets the drawn arm is at, and its turn
            const float t = std::clamp(interp::pass_t(), 0.0f, 1.0f);
            for (int side = 0; side < 2; ++side) {
                FrameHand& f = g_frameHand[side];
                const auto& lag = g_lag[side];
                f.valid = false;
                if (!lag.valid || !g_input.valid || !g_input.hand[side].active) continue;
                const Vec3 offset = to_game_dir(g_map, g_input.hand[side].grip.position - v.head.position) * world_scale();
                const Vec3 now = head.position + offset * lag.k;
                const Vec3 drawn = lerp(lag.prev, lag.cur, t);
                f.delta = now - drawn;
                const Quat ctrlNow = to_game_orientation(g_map, g_input.hand[side].aim.orientation);
                f.turn = normalize(ctrlNow * conjugate(slerp(lag.prevRot, lag.curRot, t)));
                f.haveHandNow = lag.haveHand;
                if (lag.haveHand) f.handNow = normalize(ctrlNow * conjugate(lag.curRot) * lag.handRot);
                // (more: a cut or a teleport, not a lag. At 40, running while turning the view went past it (68 to 70
                // units measured in the headset), and those passes drew the hand, and the sword in it, as the animation
                // has it: the sword flickered; cuts and teleports are hundreds of units and more)
                f.valid = finite(f.delta) && length(f.delta) < kFrameHandCut;
                if (!f.valid && finite(f.delta)) {  // (diagnostics: such passes draw the hand as the animation has it)
                    ++g_frameHandFar;
                    g_frameHandFarMax = std::max(g_frameHandFarMax, length(f.delta));
                }
                f.serial = g_passSerial;
            }
        }
        // (diagnostics) WWHD_VR_TRACE_LAG=1: the controller's target now against the drawn one, every 2 s
        if (static const bool traceLag = [] { const char* e = std::getenv("WWHD_VR_TRACE_LAG"); return e && *e == '1'; }();
            traceLag && g_input.valid) {
            static double sum[2] = {}, worst[2] = {};
            static int frames = 0;
            static auto last = std::chrono::steady_clock::now();
            const float t = interp::pass_t();
            for (int side = 0; side < 2; ++side) {
                const auto& lag = g_lag[side];
                if (!lag.valid || !g_input.hand[side].active) continue;
                const Vec3 offset = to_game_dir(g_map, g_input.hand[side].grip.position - v.head.position) * world_scale();
                const Vec3 now = head.position + offset * lag.k;
                const Vec3 drawn = t < 1.0f ? lerp(lag.prev, lag.cur, t) : lag.cur;
                const double m = double(length(now - drawn) / std::max(1.0f, world_scale()));
                sum[side] += m;
                worst[side] = std::max(worst[side], m);
            }
            ++frames;
            if (std::chrono::steady_clock::now() - last > std::chrono::seconds(2)) {
                last = std::chrono::steady_clock::now();
                LOG("[vr] hand lag: the drawn hand's target behind the controller's: left mean %.1f max %.1f cm, right mean "
                    "%.1f max %.1f cm (%d frames)", sum[0] / frames * 100.0, worst[0] * 100.0, sum[1] / frames * 100.0,
                    worst[1] * 100.0, frames);
                sum[0] = sum[1] = worst[0] = worst[1] = 0;
                frames = 0;
            }
        }
    } else {
        // events, title, file select: the game's camera position, turned by the head relative to where it
        // looked when the event began (a cut is a cut; no motion of the game camera is added to the head).
        // Menu scenes (title, file select) are seen from 50 % farther (vr.menuZoomOut) along the same line.
        if (!c.cutsceneFollow) return;
        if (!g_head.game_camera()) g_head.begin_game_camera(yaw_of(v.head.orientation));
        const Vec3 up = ld_vec(cam + kCamUp);
        if (!finite(eye) || !finite(center) || !finite(up) || length(center - eye) < 1e-3f) return;
        const bool menu = camera_kind(false, stage_name()) == CameraKind::Menu;
        head = g_head.game_camera_view(eye, center, up, v.head.orientation);
        head.position = menu_eye(eye, center, menu ? c.menuZoomOut : c.cutsceneZoomOut);
    }
    if (gameplay && !g_thirdPerson.load()) {
        static bool loggedNear = false;
        if (!loggedNear) {
            loggedNear = true;
            LOG("[vr] first person: the game camera's near plane %.2f, far %.0f units", double(ldf32(cam + 0xCC)),
                double(ldf32(cam + 0xD0)));
        }
    }
    const LookAt la = look_at(head, 100.0f);
    if (!finite(la.eye) || !finite(la.center) || !finite(la.up)) return;
    // (docs/VR_DLSS.md, "History resets") a jump of the drawn view no head or walk makes in a frame (a recentre, a snap
    // turn, an event camera taking over, a teleport): DLSS starts new histories on this frame. Only while DLSS runs
    if (render::view_cuts_wanted()) {
        static Pose lastDrawn{};
        static uint32_t lastCam = 0;
        if (lastCam && (cam != lastCam || motion::view_jump(length(head.position - lastDrawn.position),
                                                            angle_between(lastDrawn.orientation, head.orientation),
                                                            g_map.unitsPerMetre > 0 ? g_map.unitsPerMetre : world_scale())))
            render::note_view_cut();
        lastDrawn = head;
        lastCam = cam;
    }
    for (int i = 0; i < 9; ++i) saved_[i] = ld32(cam + kCamEye + 4 * i);
    saved_[9] = ld16(cam + kCamBank);
    st_vec(cam + kCamEye, la.eye);
    st_vec(cam + kCamCenter, la.center);
    st_vec(cam + kCamUp, la.up);
    st16(cam + kCamBank, 0);
    cam_ = cam;
    written_ = true;
    g_head.drawn(head.orientation);
    // these views are what this frame shows: they go with its swap to the compositor
    set_frame_views(store_views(rendered, double(interp::logic_steps()) + double(interp::pass_t())));
}

CameraOverride::~CameraOverride() {
    GameWorkTimer workTimer(GameWork::Camera);  // (docs/VR_PERFORMANCE.md: the VR work on the game thread)
    if (!written_) return;
    // the game's camera logic continues from its own state, never from the head
    for (int i = 0; i < 9; ++i) st32(cam_ + kCamEye + 4 * i, saved_[i]);
    st16(cam_ + kCamBank, uint16_t(saved_[9]));
}

// ---- Link
// diagnostics (WWHD_VR_TRACE_HELD=1, at most once a second per stage): the held item (mpEquipItemModel) against
// Link's hand joints, and whether its matrices changed between his execute and his draw
uint32_t g_heldHash = 0;
// diagnostics (WWHD_VR_TRACE_HAND=1): Link's right hand joint at his execute and at each draw pass, against his
// position; once a second: how far the hand had moved at the draws since the execute, and how far Link had
void trace_hand_joint(uint32_t link, bool draw) {
    static const bool on = [] { const char* e = std::getenv("WWHD_VR_TRACE_HAND"); return e && *e == '1'; }();
    if (!on || !link) return;
    const uint32_t body = ld32(link + kBodyModel);
    const uint32_t bb = body ? ld32(body + kMtxBlock) : 0;
    const uint32_t bm = bb ? ld32(bb + kMtxBlockMatrices) : 0;
    if (!bm) return;
    const Vec3 hand{float(ldf32(bm + 12 * 48 + 12)), float(ldf32(bm + 12 * 48 + 28)), float(ldf32(bm + 12 * 48 + 44))};
    const Vec3 pos = ld_vec(link + kPos);
    static Vec3 execHand, execPos;
    static int draws = 0, moved = 0;
    static double handMove = 0, posMove = 0, eyeDelta = 0;
    if (!draw) {
        execHand = hand;
        execPos = pos;
        return;
    }
    ++draws;
    const float d = length(hand - execHand);
    moved += d > 0.01f;
    handMove += d;
    posMove += length(pos - execPos);
    const float t = interp::pass_t();
    eyeDelta += double(t);
    static auto last = std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() - last < std::chrono::seconds(1)) return;
    last = std::chrono::steady_clock::now();
    LOG("[vr] hand joint: %d draws, %d with the hand moved since the execute (%.2f units on average); Link moved %.2f; "
        "mean pass fraction %.2f", draws, moved, handMove / std::max(1, draws), posMove / std::max(1, draws),
        eyeDelta / std::max(1, draws));
    draws = moved = 0;
    handMove = posMove = eyeDelta = 0;
}
void trace_held(uint32_t link, const char* stage) {
    static const bool on = [] { const char* e = std::getenv("WWHD_VR_TRACE_HELD"); return e && *e == '1'; }();
    if (!on || !link) return;
    const uint32_t model = held_model(link);
    const uint32_t block = model ? ld32(model + kMtxBlock) : 0;
    const uint32_t mtx = block ? ld32(block + kMtxBlockMatrices) : 0;
    const uint16_t count = block ? ld16(block + 0x2C) : 0;
    uint32_t hash = 0;
    for (uint32_t i = 0; mtx && i < uint32_t(count) * 12 && i < 32 * 12; ++i) hash = hash * 31 + ld32(mtx + i * 4);
    const bool draw = stage[0] == 'd';
    static int drawn = 0, changed = 0;
    if (draw) {
        ++drawn;
        changed += hash != g_heldHash;
    } else {
        g_heldHash = hash;
    }
    static auto last = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (draw || now - last < std::chrono::seconds(1)) return;
    last = now;
    const uint32_t body = ld32(link + kBodyModel);
    const uint32_t bb = body ? ld32(body + kMtxBlock) : 0;
    const uint32_t bm = bb ? ld32(bb + kMtxBlockMatrices) : 0;
    auto jp = [](uint32_t m, int j) { return Vec3{float(ldf32(m + j * 48 + 12)), float(ldf32(m + j * 48 + 28)), float(ldf32(m + j * 48 + 44))}; };
    if (!mtx || !bm) {
        LOG("[vr] held: none (equip model %08X)", model);
        return;
    }
    const Vec3 root = jp(mtx, 0);
    const int d = dominant_side(config());
    LOG("[vr] held: model %08X, %u joints, root %.1f from hand 8, %.1f from hand 12; game's hand %d, %s, drawn in hand "
        "%d (%s-handed); sword tip %.1f / root %.1f from it; matrices changed between execute and draw in %d of %d draws",
        model, unsigned(count), double(length(root - jp(bm, 8))), double(length(root - jp(bm, 12))), g_held.gameHand,
        g_held.sword ? "a sword" : "not a sword", g_held.gameHand < 0 ? -1 : kArmJoints[g_jointSet[d]][2],
        d ? "right" : "left", double(length(ld_vec(link + kSwordTop) - root)), double(length(ld_vec(link + kSwordRoot) - root)),
        changed, drawn);
    drawn = changed = 0;
}

// game functions for the physical sword (TPVR's startVrPhysicalSwordCut / updateVrPhysicalSwordCut)
const uint32_t kGameInfo = GC(0x025200D4);            // dComIfG_getGameInfo: + 0x26A4 the collision (cCcS),
constexpr uint32_t kCcS = 0x26A4, kMassS = 0x4EF8;   // + 0x4EF8 the mass collision (grass, bushes)
const uint32_t kCcSSet = GC(0x0200E240);              // cCcS::Set(ccs, obj)
const uint32_t kMassSet = GC(0x02516C14);             // dCcMassS_Mng::Set(mass, obj, 1): as Link's own attacks
const uint32_t kResetAtHit = GC(0x02516138);          // dCcD_GObjInf::ResetAtHit(obj)
const uint32_t kNormalCutAtParam = GC(0x023E5B0C);    // daPy_lk_c::setNormalCutAtParam(link, cutType)
const uint32_t kEnemyWeaponAtParam = GC(0x023ED2CC);  // daPy_lk_c::setEnemyWeaponAtParam(link, strong)
const uint32_t kDeleteEquipItem = GC(0x023DC7AC);     // daPy_lk_c::deleteEquipItem(link, sound)
constexpr uint32_t kCutRight = 3, kCutLeft = 4;       // procCutR_init / procCutL_init's cut types

// ---- the Skull Hammer's swing (vr_sword.h, docs/VR_SKULL_HAMMER.md)
constexpr uint32_t kFrameCtrlUnder = 0x5898;  // daPy_lk_c::mFrameCtrlUnder[0] (J3DFrameCtrl: mRate +0, mFrame +4)
constexpr uint32_t kCutType = 0x3AC;          // daPy_py_c::mCutType (0x11 the side swing, 0x12 the front swing)
// after Link's execute: the procedure the hammer's button started for a swing the hand made, its windup skipped once
// (hammer_windup_skip). Not for the button pressed on the controller (the game's own timing) or after 8 steps
void hammer_windup(uint32_t link, uint64_t step) {
    {
        // each of the hammer's swing procedures as it starts (and the attack set), at most a few lines per swing
        static uint32_t loggedProc = 0;
        static bool loggedAttack = false;
        const uint32_t proc = ld32(link + kCurProc);
        const bool attack = (ld32(link + kAtCps) & 1) != 0;
        if ((proc != loggedProc || attack != loggedAttack) && (hammer_swing_proc(proc) || hammer_swing_proc(loggedProc)))
            LOG("[vr] hammer: procedure %X at step %llu (attack %s, cut type %02X)", unsigned(proc),
                (unsigned long long)step, attack ? "set" : "not set", unsigned(ld8(link + kCutType)));
        loggedProc = proc;
        loggedAttack = attack;
    }
    if (g_hammerSkipped) return;
    if (step > g_hammerPressStep + 8 || ld16(link + 0x69B0) != kItemSkullHammer) {
        g_hammerSkipped = true;
        LOG("[vr] hammer: no swing started for the press at step %llu (procedure %X)",
            (unsigned long long)g_hammerPressStep, unsigned(ld32(link + kCurProc)));
        return;
    }
    const uint32_t proc = ld32(link + kCurProc), fc = link + kFrameCtrlUnder;
    const float frame = float(ldf32(fc + 4));
    const WindupSkip k = hammer_windup_skip(proc, frame);
    if (!k.apply) return;
    if (k.endAnimation) stf32(fc, 0.0f);
    else stf32(fc + 4, k.frame);
    g_hammerSkipped = true;
    LOG("[vr] hammer: %s at step %llu (pressed at %llu): %s", proc == kProcHammerSideSwing ? "side swing" : "front swing",
        (unsigned long long)step, (unsigned long long)g_hammerPressStep,
        k.endAnimation ? "its windup ended" : "moved to its hits");
}
// the hammer's button (and a side swing's stick) for the steps a swing presses it
void hammer_press(input::PadState& s, uint64_t step) {
    if (step < g_bFrom || step >= g_bTo) return;
    const uint32_t l = g_link.link;
    if (!l || !g_link.valid || game_item(l) != kItemSkullHammer) return;
    uint8_t items[3];
    for (int i = 0; i < 3; ++i) items[i] = ld8(play() + kPlaySelectItems + i);
    const int b = item_button(items, uint8_t(kItemSkullHammer));
    if (b < 0) {
        static uint64_t logged = ~0ull;
        if (logged != g_bFrom) LOG("[vr] hammer: on no item button (X, Y, R hold %02X %02X %02X): no swing", items[0], items[1], items[2]);
        logged = g_bFrom;
        return;
    }
    s.buttons |= b == 0 ? pad::kX : b == 1 ? pad::kY : pad::kR;
    if (g_hammerStick != 0) {
        s.lx = g_hammerStick;
        s.ly = 0;
    }
}
// a thrust's press (docs/VR_HELD_ITEMS.md, "Thrusts"): the stick held forward with its B, so the game's choice of cut
// (daPy_lk_c::changeCutProc: the stick's direction from Link's facing, or from the target when locked on) is its
// forward thrust (procCutF_init). After the movement, so the stick is that alone for its two steps
void thrust_press(input::PadState& s, uint64_t step) {
    if (step < g_bFrom || step >= g_bTo || !g_bThrust || g_bHammer) return;
    s.lx = 0;
    s.ly = 1.0f;
}
// a downward swing locked on to a target: the game's vertical slash (daPy_lk_c::changeCutProc: locked on, B with the stick
// in the middle, procCutA). The left stick is held in the middle for the press's two steps; walking while swinging gave
// the stick's direction, and with it a side or forward cut (asked for: a vertical swing locked on is the vertical
// slash; docs/VR_CONTROLS.md, "The vertical slash")
void vertical_press(input::PadState& s, uint64_t step) {
    if (step < g_bFrom || step >= g_bTo || g_bThrust || g_bHammer || g_bParry || g_bJump) return;
    const uint32_t l = g_link.link;
    if (!l || !g_link.valid || !ld32(l + kLockOnActor)) return;
    // (a side swing locked on: plain B with the stick as the player holds it, the game's normal combo slash; it held the
    // stick to the swing's side for the side cut until the user asked for the normal attack, 2026-11-04)
    if (g_bVertical) s.lx = s.ly = 0;
}
// test aid (no headset needed): WWHD_VR_TEST_HAMMER=step:kind[,step:kind...] (kind v: a downward swing, l / r: a swing
// to the left / right) makes a full swing of the hammer at those logic steps, as the sword hand's would
void test_hammer(uint64_t step) {
    struct Swing {
        uint64_t step;
        char kind;
    };
    static const std::vector<Swing> swings = [] {
        std::vector<Swing> out;
        const char* e = std::getenv("WWHD_VR_TEST_HAMMER");
        while (e && *e) {
            unsigned long long st = 0;
            char k = 0;
            if (std::sscanf(e, "%llu:%c", &st, &k) == 2 && (k == 'v' || k == 'l' || k == 'r')) out.push_back({st, k});
            e = std::strchr(e, ',');
            if (e) ++e;
        }
        return out;
    }();
    static size_t next = 0;
    while (next < swings.size() && step >= swings[next].step) {
        const Swing w = swings[next++];
        if (!g_held.hammer || g_held.gameHand < 0) {
            LOG("[vr] hammer test: no hammer in the hand at step %llu", (unsigned long long)step);
            continue;
        }
        g_bFrom = step;
        g_bTo = step + 2;
        g_bThrust = false;
        g_hammerStick = hammer_swing_stick(w.kind != 'v', w.kind == 'r' ? 1 : -1);
        g_hammerPressStep = step;
        g_hammerSkipped = false;
        LOG("[vr] hammer test: a full %s at step %llu",
            w.kind == 'v' ? "downward swing" : w.kind == 'r' ? "swing to the right" : "swing to the left", (unsigned long long)step);
    }
}

// A picked-up weapon's (a stick's) tracked base frame: its handle at Link's sword hand as the arm IK posed it
// (mirrored from the game's hand), turned as the controller holds it (g_swordRot), moved by vr.stickOffset*. The frame
// its draw puts it in (weapon_drawing), and, from Link's execute on, its base matrix (place_weapon_base). `rot`, `at`:
// the frame already chosen for the sword hand's grip; false (unchanged) when the handle is not known yet
bool weapon_target(uint32_t link, const Config& c, uint32_t body, Quat& rot, Vec3& at) {
    if (!g_held.handleKnown || !g_held.haveLast) return false;
    const int d = dominant_side(c);
    const int swordHand = kArmJoints[g_jointSet[d]][2];
    const Joint hand = read_joint(body, swordHand);
    if (!finite(hand.pos)) return false;
    const int set = swordHand == kArmJoints[1][2] ? 1 : 0;
    at = hand.pos + rotate(hand.rot, g_held.handleLocal[set]) - rotate(rot, g_held.baseLocal);
    // moved by vr.stickOffset* in the controller's grip space (a little right and towards you by default)
    if (!c.leftHanded && g_input.valid && g_input.hand[d].active)  // right-handed play: where they were set
        at = at + to_game_dir(g_map, rotate(g_input.hand[d].grip.orientation,
                                            Vec3{c.stickOffsetX, c.stickOffsetY, c.stickOffsetZ})) * world_scale();
    return finite(at);
}
// A picked-up weapon's base matrix at its tracked frame, right after Link's execute (whose setItemModel set it from
// his game hand). The weapon's own execute calculates its model from that base, and everything of it follows: its tip
// (daBoko_c's top), what touches fire there and lights it, its flame's particles, and the flame Link's
// getBokoFlamePos reports to torches. Only its draw had been moved to the hand: it caught fire, and showed its flame,
// where the game held it, not where it was seen (reported in the headset: a stick touched to a fire did not light)
void place_weapon_base(uint32_t link, const Config& c) {
    if (ld16(link + 0x69B0) != kItemEnemyWeapon || !g_held.sword || g_held.gameHand < 0 || !c.trackedSword || !c.armIK ||
        g_thirdPerson.load() || !play_view())
        return;
    const uint32_t actor = ld32(link + kKeptEquipActor);
    const uint32_t model = actor ? ld32(actor + kBokoModel) : 0;
    if (!model || model != g_held.model) return;
    uint16_t bn = 0;
    const uint32_t body = matrices_of(ld32(link + kBodyModel), bn);
    if (!body || bn <= 12) return;
    const Grip& grip = g_grip[kArmJoints[g_jointSet[dominant_side(c)]][2] == kArmJoints[1][2] ? 1 : 0];
    if (!grip.valid) return;
    const Joint hand = read_joint(body, kArmJoints[g_jointSet[dominant_side(c)]][2]);
    Quat rot = g_swordRotValid ? g_swordRot : normalize(hand.rot * grip.rot);
    Vec3 at;
    if (!weapon_target(link, c, body, rot, at)) return;
    float m[12];
    for (int i = 0; i < 12; ++i) m[i] = float(ldf32(model + kModelBaseMtx + i * 4));
    Joint base = joint_from_matrix(m);  // (its own scale kept)
    if (!finite(base.pos)) return;
    base.rot = rot;
    base.pos = at;
    matrix_from_joint(base, m);
    for (int i = 0; i < 12; ++i) stf32(model + kModelBaseMtx + i * 4, m[i]);
    g_held.baseWritten = at;
    static uint32_t loggedActor = 0;
    if (loggedActor != actor) {
        loggedActor = actor;
        LOG("[vr] held weapon: its base at the tracked hand from Link's execute on (its tip, fire and flame follow it)");
    }
}

// after Link's step: the bomb he carries in the sword hand, or the one just thrown given the hand's velocity
// (docs/VR_HELD_ITEMS.md, "Bombs in the hand")
void bomb_step(uint32_t link) {
    static uint32_t lastBomb = 0;
    const bool fp = running() && !g_thirdPerson.load() && first_person_play();
    const uint32_t bomb = fp ? carried_bomb(link) : 0;
    // thrown this step (Link's procGrabThrow set the game's lob, 19 forward and 34 up, and let go of it): the hand's
    // velocity instead, along its own heading, held within the throw's limits
    if (lastBomb && bomb != lastBomb && g_bombThrow.pending && g_bombThrow.bomb == lastBomb &&
        interp::logic_steps() - g_bombThrow.step < 60) {
        const float speedF = float(ldf32(lastBomb + kSpeedF)), speedY = float(ldf32(lastBomb + kSpeed + 4));
        if (std::fabs(speedF - 19.0f) < 0.5f && std::fabs(speedY - 34.0f) < 0.5f) {
            const Vec3 v = g_bombThrow.vel * (kBombThrowGain / 30.0f);  // (a second's units to a step's)
            const float forward = std::min(std::sqrt(v.x * v.x + v.z * v.z), kBombMaxForward);
            // (further, asked for in the headset: at 2x a throw went 17-21 forward and nearly flat)
            const float up = std::clamp(v.y + kBombLoft, kBombMinUp, kBombMaxUp);
            if (finite(v) && forward > 1.0f) st16(lastBomb + kHeading, uint16_t(radians_to_s16(std::atan2(v.x, v.z))));
            stf32(lastBomb + kSpeedF, forward);
            stf32(lastBomb + kSpeed + 4, up);
            LOG("[vr] bomb: thrown with the hand (%.1f forward, %.1f up a step; the game's: 19, 34)", double(forward),
                double(up));
        }
        g_bombThrow.pending = false;
    }
    lastBomb = bomb;
    if (!bomb) return;
    // in the sword hand as posed this step (its arm follows the controller while a bomb is carried), a little out from
    // the palm along where the controller points
    uint16_t n = 0;
    const uint32_t body = matrices_of(ld32(link + kBodyModel), n);
    const Config c = config();
    const int d = dominant_side(c);
    const int hand = kArmJoints[g_jointSet[d]][2];
    if (!body || n <= uint32_t(hand) || !g_input.valid || !g_input.hand[d].active) return;
    const Vec3 at = read_joint(body, hand).pos +
                    to_game_dir(g_map, rotate(g_input.hand[d].grip.orientation, Vec3{0, 0, -1})) * kBombInHand;
    if (!finite(at)) return;
    stf32(bomb + kPos, at.x);
    stf32(bomb + kPos + 4, at.y);
    stf32(bomb + kPos + 8, at.z);
    static uint32_t logged = 0;
    if (logged != bomb) {
        logged = bomb;
        LOG("[vr] bomb: in the sword hand (squeeze its grip, let go to throw)");
    }
}

// An arrow let go before the string was drawn (vr_bow.h drop): after the game's shot, each step for at most
// kArrowFallSteps its speed is set (an arrow in flight only moves by its speed: procMove), so it drops out of the hand
// and lands at your feet: kArrowFallForward along the bow, and falling from rest by kArrowFallGravity a step. Only
// while it is still that arrow (an arrow, fpcNm_ARROW_e, not on the bow again)
void arrow_fall_step(uint32_t link) {
    ArrowFall& f = g_arrowFall;
    if (!f.arrow) return;
    const bool same = int16_t(ld16(f.arrow + kActorName)) == 0x1D8 && ld32(link + kKeptEquipActor) != f.arrow;
    if (!same || ++f.steps > kArrowFallSteps) {
        f = {};
        return;
    }
    f.vy = std::max(f.vy - kArrowFallGravity, -kArrowFallMax);
    const Vec3 fwd = length(f.forward) > 1e-3f ? normalize(f.forward) * kArrowFallForward : Vec3{};
    stf32(f.arrow + kSpeed, fwd.x);
    stf32(f.arrow + kSpeed + 4, f.vy);
    stf32(f.arrow + kSpeed + 8, fwd.z);
    stf32(f.arrow + kSpeedF, 0.0f);
}

void link_executed(uint32_t link, Cpu* cpu) {
    GameWorkTimer workTimer(GameWork::Execute);  // (docs/VR_PERFORMANCE.md: the VR work on the game thread)
    if (link) bomb_step(link);
    if (link) arrow_fall_step(link);
    if (!link) return;
    // (the spin attacks' view turn, asked for: "have the camera follow the sword animation as it turns") the game's own
    // sword tip as his animation put it, taken before this hook writes the tracked blade over it (below): its direction
    // from him, flat
    // Spin speed (vr.spinSpeed; docs/VR_CONTROLS.md, "Spin speed"): at the start of the spin attack, the Hurricane Spin
    // and its end (each sets its animation then), the rate of his lower and upper body's animation (mFrameCtrlUnder[0],
    // mFrameCtrlUpper[0]: mRate first) scaled once; in the Hurricane Spin, the turn the game adds each step (m34EC)
    // scaled by the same. The view follows that frame and that turn (the spin branch), so the animation, his model's
    // turn and the view's stay in step
    {
        constexpr uint32_t kUnderRate = 0x5898, kUpperRate = 0x58B8, kSpinTurn = 0x6934;
        static uint32_t lastProc = 0;
        static int16_t lastTurn = 0;
        const uint32_t proc = ld32(link + kCurProc);
        const float k = std::clamp(config().spinSpeed, 0.5f, 1.0f);
        if (spin_attack_proc(proc) && proc != lastProc && k < 0.999f) {
            for (uint32_t at : {kUnderRate, kUpperRate}) {
                const float rate = float(ldf32(link + at));
                if (std::isfinite(rate) && rate > 0.0f) stf32(link + at, rate * k);
            }
            LOG("[vr] spin speed %.2fx: action 0x%X's animation slowed", double(k), proc);
        }
        if (proc == 0x56 && k < 0.999f) {
            const int16_t turn = int16_t(ld16(link + kSpinTurn));
            if (lastProc == 0x56) {
                const int16_t scaled = int16_t(lastTurn + int16_t(std::lround(float(int16_t(turn - lastTurn)) * k)));
                st16(link + kSpinTurn, uint16_t(scaled));
                lastTurn = scaled;
            } else {
                lastTurn = turn;
            }
        }
        lastProc = proc;
    }
    if (spin_attack_proc(ld32(link + kCurProc))) {
        const Vec3 d = ld_vec(link + kSwordTop) - ld_vec(link + kPos);
        g_gameSwordYaw.store(finite(d) && d.x * d.x + d.z * d.z > 100.0f ? std::atan2(d.x, d.z) : NAN,
                             std::memory_order_relaxed);
    } else {
        g_gameSwordYaw.store(NAN, std::memory_order_relaxed);
    }
    const Config c = config();
    std::unique_lock<std::mutex> lock(g_mutex);
    test_wheel(link);
    (void)picto_view();  // (its log, also outside VR)
    const uint64_t step = interp::logic_steps();
    const Vec3 pos = ld_vec(link + kPos);
    if (!finite(pos)) return;
    if (g_link.link != link || !g_link.valid) {
        g_link = {};
        g_link.link = link;
        g_link.prevPos = g_link.pos = pos;
        g_link.turnPrev = g_link.turnCur = g_head.turn_total();
        g_link.valid = true;
        g_head.step_new_link();  // (not blended: his position is not either)
    } else if (step != g_link.step) {
        g_link.prevPos = g_link.pos;
        g_link.pos = pos;
        g_link.roomPush = g_head.step(pos);  // (walls: the origin takes what he took)
        g_link.prevEyeOffset = g_link.eyeOffset;  // (the eye point as drawn at the end of the step before: anchor)
        g_link.prevEyeHeight = g_link.eyeHeight;
        g_link.prevHeading = g_link.heading;
        g_link.eyeBlendSeeded = g_link.offsetSeeded && g_link.heightSeeded;
        g_link.turnPrev = g_link.turnCur;
        g_link.turnCur = g_head.turn_total();  // (his facing takes this step's view: link_executing)
    } else {
        g_link.pos = pos;  // the same step again (true 60's preview): only the latest position
        g_link.roomPush = g_link.roomPush + g_head.step_again(pos);
    }
    // his own movement this step (the game's), without the room-scale move: standing still is still standing
    const Vec3 gameMoved = g_link.pos - g_link.prevPos - g_link.roomPush;
    // Link's eye height without the bob of his animation: a slow follow of his animated eyes (crawling,
    // swimming and climbing change it for good, walking only for a moment)
    const float eye = float(ldf32(link + kEyePos + 4)) - pos.y;
    // the water's surface where he is (swimming: the view stays above it, head_in_game); only in the actions swimming
    // is among (0x2B..0x3F, as below; swimming 0x37), so a water height left from before cannot hold the view on land
    {
        const uint32_t proc = ld32(link + kCurProc);
        g_link.waterY = proc >= 0x2B && proc <= 0x3F ? float(ldf32(link + kWaterY)) : -INFINITY;
    }
    // only a real change of posture moves the camera: the idle animation's breathing (a few units) does not
    const float heightBefore = g_link.eyeHeight;
    const Vec3 offsetBefore = g_link.eyeOffset;
    // Standing in place, the camera does not follow his animations' height (asked for in the headset: it bobbed up and
    // down on its own while he stood): drawing and sheathing the sword, taking out items, conducting, a swing, the end
    // of an event moved his eyes 20 units and more for a moment, past the dead zone, and the camera followed them
    // (measured in a session: 16 of 42 still windows without lifting or carrying, 1 to 7 units a step). Only then: his
    // feet still and his action not a posture the view goes with (crouching and crawling 0x0C..0x12; hanging,
    // swimming, ladders and climbing 0x2B..0x3F). In those, moving, and while catching up after one of them (getting
    // up), the eye height follows as before. A change that lasts while standing (kLastingAway units for kLastingTime s,
    // an action this does not name) is followed too, and logged. Only the VR view's height (and what is measured from
    // it: the arms' reach, the swing's mapping) changes; nothing of the game's
    constexpr float kLastingAway = 15.0f, kLastingTime = 1.0f;
    if (std::isfinite(eye) && eye > 10.0f && eye < 250.0f && step != g_link.step) {
        if (!g_link.heightSeeded) g_link.eyeHeightHeld.reset(eye);
        g_link.heightSeeded = true;
        const uint32_t proc = ld32(link + kCurProc);
        const bool still = length(gameMoved) < 0.5f;
        // The view's height is constant (asked for after a play test, 2026-11-04: "his height should be constant the
        // whole time"; with the shield up, locked on, his battle stance lowered his eyes as he moved and the view went
        // down with them, as if crouching; standing, lasting changes of 52, -21 and 26 units were followed): his
        // standing eye height, taken the first time he stands still (daPyProc_WAIT_e) after a load, for every action
        // but the postures that put his body elsewhere: crawling (0x0D..0x12; not the crouching guard 0x0C, a stance)
        // and hanging, swimming, ladders and climbing (0x2B..0x3F), which the view follows and comes back from
        static uint32_t standLink = 0;
        static float standEye = NAN;
        if (standLink != link) {
            standLink = link;
            standEye = NAN;
        }
        if (!std::isfinite(standEye) && proc == 4 && still) {
            standEye = eye;
            LOG("[vr] first person: the view's height held at his standing eye height, %.1f units", double(eye));
        }
        const bool posture = (proc >= 0x0D && proc <= 0x12) || (proc >= 0x2B && proc <= 0x3F);
        if (std::isfinite(standEye) && !posture && !parry_move_proc(proc)) {
            g_link.eyeHeightHeld.update(standEye, 6.0f, kStepSeconds, 0.35f);  // (eased back after a posture)
            g_link.heightFollow = false;
            g_link.heightAway = 0;
        } else if (parry_move_proc(proc)) {
            // (a parry's flip or roll: the view keeps his standing eye height above his feet, which carry the jump;
            // his head turning over in the move is not followed: docs/VR_CONTROLS.md, "Parry attacks")
        } else if (!still || posture || g_link.heightFollow) {
            // as before (and after a posture until the view has caught up with him)
            g_link.eyeHeightHeld.update(eye, 6.0f, kStepSeconds, 0.35f);
            g_link.heightFollow = posture || (still && g_link.eyeHeightHeld.moving);
            g_link.heightAway = 0;
        } else {
            // standing: held against his animations
            const float away = std::fabs(eye - g_link.eyeHeightHeld.value);
            if (away > g_link.heightPeak) {
                g_link.heightPeak = away;
                g_link.heightPeakProc = proc;
            }
            // (not in a conversation in first person: his talking, action 0xAA, moves his eyes 31 to 35 units up and down
            // for seconds at a time, and the view followed each: Quest 3 log, 2026-11-03, six jumps of about 30 cm in one
            // conversation. The height is held until it ends)
            g_link.heightAway = away > kLastingAway && !talk_first_person() ? g_link.heightAway + kStepSeconds : 0.0f;
            if (g_link.heightAway >= kLastingTime) {
                LOG("[vr] first person: eye height follows a lasting change of %.1f units while standing (action 0x%X)",
                    double(eye - g_link.eyeHeightHeld.value), proc);
                g_link.heightFollow = true;
                g_link.heightAway = 0;
                g_link.eyeHeightHeld.update(eye, 6.0f, kStepSeconds, 0.35f);
            } else {
                g_link.eyeHeightHeld.reset(g_link.eyeHeightHeld.value);  // (nothing half-followed kept going)
            }
        }
        g_link.eyeHeight = g_link.eyeHeightHeld.value;
    }
    const float headingBefore = g_link.heading;
    g_link.heading = link_frame_heading(link);
    if (!g_link.headingSeeded) {  // (a new Link: not blended from heading 0)
        g_link.prevHeading = g_link.heading;
        g_link.headingSeeded = true;
    }
    // WWHD_VR_TRACE_LEDGE=1 (also without a headset): his action at each change, and on a ladder or ledge twice a
    // second, with his position, his model's facing, the way he moves and the frame the view uses (degrees)
    static const bool traceLedge = [] { const char* e = std::getenv("WWHD_VR_TRACE_LEDGE"); return e && *e == '1'; }();
    if (traceLedge && step != g_link.step) {
        static uint32_t lastProc = ~0u;
        static int tick = 0;
        const uint32_t proc = ld32(link + kCurProc);
        if (proc != lastProc || (rig_follows_link_proc(proc) && ++tick % 15 == 0)) {
            const float deg = 180.0f / kPi;
            LOG("[vr] ledge facts: proc 0x%X%s at %.0f %.0f %.0f facing %.0f travel %.0f frame %.0f", proc,
                proc != lastProc ? " (new)" : "", double(pos.x), double(pos.y), double(pos.z),
                double(s16_to_radians(int16_t(ld16(link + kShapeAngleY))) * deg),
                double(s16_to_radians(int16_t(ld16(link + kHeading))) * deg), double(g_link.heading * deg));
            lastProc = proc;
        }
    }
    // where his eyes are horizontally: the head joint of this step's animation (the first-person collapse happens
    // later, in his draw) plus his eyes' distance in front of it along his heading, filtered so the bob of running
    // does not sway the view. That distance is the game's own eye point (eyePos, whose height is used above) in front
    // of the head joint, measured while he looks ahead: about 18 units (his big head; the joint is at its back). It was
    // a fixed 4 units: the camera sat in the middle of his head, 14 units behind his eyes, and looking down showed his
    // shoulders and chest from straight above, too close to see the rest of his body (docs/VR_FIRST_PERSON_CAMERA.md).
    if (step != g_link.step) {
        const uint32_t body = ld32(link + kBodyModel);
        const uint32_t block = body ? ld32(body + kMtxBlock) : 0;
        const uint32_t mtx = block ? ld32(block + kMtxBlockMatrices) : 0;
        if (mtx && ld16(block + 0x2C) > kHeadJointIndex) {
            const Vec3 head{float(ldf32(mtx + kHeadJointIndex * 48 + 12)), float(ldf32(mtx + kHeadJointIndex * 48 + 28)),
                            float(ldf32(mtx + kHeadJointIndex * 48 + 44))};
            {
                const Vec3 fwd = heading_dir(g_link.heading), right = heading_right(g_link.heading);
                const Vec3 e = ld_vec(link + kEyePos) - head;
                float sample = 0;
                if (eye_forward_sample({dot(e, fwd), dot(e, right), e.y}, sample)) {
                    if (!g_link.forwardSeeded) g_link.eyeForward.reset(sample);
                    g_link.forwardSeeded = true;
                    g_link.eyeForward.update(sample, 2.0f, kStepSeconds, 0.5f);
                }
            }
            g_link.headToEye = ld_vec(link + kEyePos) - head;  // where his animated head faces (rig_facing)
            const float eyeForward = g_link.forwardSeeded ? g_link.eyeForward.value : kEyeForwardDefault;
            Vec3 target = Vec3{head.x - pos.x, 0, head.z - pos.z} + heading_dir(g_link.heading) * eyeForward;
            if (finite(target) && length(target) < 40.0f) {
                const Vec3 fwd = heading_dir(g_link.heading), right = heading_right(g_link.heading);
                const Vec3 local{dot(target, right), 0, dot(target, fwd)};
                if (!g_link.offsetSeeded) g_link.eyeOffsetHeld.reset(local);
                g_link.offsetSeeded = true;
                // (a parry's flip or roll: his eyes kept in front of him as they were, not his head's arc)
                if (!parry_move_proc(ld32(link + kCurProc))) g_link.eyeOffsetHeld.update(local, 4.0f, kStepSeconds, 0.2f);
                // plus the player's own setting (vr.eyeForward, metres; 0 = his eyes), after the hold so that any
                // change of it moves the view at once
                const Vec3 eyesNow = right * g_link.eyeOffsetHeld.value.x +
                                     fwd * (g_link.eyeOffsetHeld.value.z + c.eyeForward * world_scale());
                // the eye point while his body turns to follow the head (Link standing): held where it was when the
                // turn began, then a glide to his eyes after it (about half a second). Turning him in place swung
                // his eyes, 18 units in front of his centre, round it with the turn: the view slid sideways with every
                // body turn (docs/VR_BODY_FOLLOW.md, "The view while the body turns")
                // ...and while the game turns him at once with his feet still (grabbing, talking, opening a chest,
                // locking on: measured 37 units of the view in one step while he picked something up, the eye point
                // swung round his centre to the other side): held the same way
                const bool feetStill = length(gameMoved) < 0.5f;
                // (turned about his eyes with the head this step: room_scale_move. Only a turn of the game's own on
                // top of the facing he was given is a jump)
                float aboutFacing = 0;
                const bool aboutEyes = g_head.take_turned_about_eyes(aboutFacing);
                const float facingFrom = aboutEyes ? aboutFacing : headingBefore;
                const bool facingJumped = feetStill && g_link.offsetSeeded &&
                                          std::fabs(wrap_radians(g_link.heading - facingFrom)) > kFacingJump;
                static int jumpsLogged = 0;
                if (facingJumped && !g_link.eyeHold && jumpsLogged < 20 && ++jumpsLogged)
                    LOG("[vr] first person: Link turned %.0f degrees at once (action 0x%X): the eye point held, then glides",
                        double(std::fabs(wrap_radians(g_link.heading - headingBefore)) * 180.0f / kPi), ld32(link + kCurProc));
                if (aboutEyes && !facingJumped) {
                    g_link.eyeOffset = eyesNow;  // (his eyes are where they were: his position moved instead)
                    g_link.eyeHold = g_link.eyeGlide = false;
                } else if ((g_body.following && (body_follow_proc(ld32(link + kCurProc)) || conducting_first_person(link))) ||
                           facingJumped) {
                    if (!g_link.eyeHold) {
                        g_link.eyeHold = true;
                        g_link.eyeHoldPoint = pos + offsetBefore;
                    }
                    g_link.eyeOffset = Vec3{g_link.eyeHoldPoint.x - pos.x, 0, g_link.eyeHoldPoint.z - pos.z};
                    g_link.eyeGlide = true;
                } else if (g_link.eyeGlide) {
                    g_link.eyeHold = false;
                    const float k = 1.0f - std::exp(-kStepSeconds / 0.15f);
                    g_link.eyeOffset = lerp(g_link.eyeOffset, eyesNow, k);
                    if (length(g_link.eyeOffset - eyesNow) < 0.25f) {
                        g_link.eyeOffset = eyesNow;
                        g_link.eyeGlide = false;
                    }
                } else {
                    g_link.eyeOffset = eyesNow;
                }
                if (!finite(g_link.eyeOffset) || length(g_link.eyeOffset - eyesNow) > 60.0f) {  // (a teleport: his eyes)
                    g_link.eyeOffset = g_link.prevEyeOffset = eyesNow;  // (not blended across it)
                    g_link.eyeHold = g_link.eyeGlide = false;
                }
                if (!g_link.eyeOffsetLogged || (g_link.forwardSeeded && !g_link.forwardLogged)) {
                    g_link.eyeOffsetLogged = true;
                    g_link.forwardLogged = g_link.forwardSeeded;
                    LOG("[vr] first person: eyes %.1f units ahead of Link's feet (head joint %.1f + %.1f forward)",
                        double(length(target)), double(length(Vec3{head.x - pos.x, 0, head.z - pos.z})), double(eyeForward));
                }
            }
        }
    }
    if (step != g_link.step && length(gameMoved) < 1e-3f) {
        g_link.stillDrift = std::max(g_link.stillDrift, std::fabs(g_link.eyeHeight - heightBefore) +
                                                            length(g_link.eyeOffset - offsetBefore));
        g_link.stillHeight = std::max(g_link.stillHeight, std::fabs(g_link.eyeHeight - heightBefore));
        static const bool traceEyes = [] { const char* e = std::getenv("WWHD_VR_TRACE_EYES"); return e && *e == '1'; }();
        if (traceEyes)
            LOG("[vr] eyes: height %.2f (animated %.2f) offset %.2f %.2f", double(g_link.eyeHeight), double(eye),
                double(g_link.eyeOffset.x), double(g_link.eyeOffset.z));
        if (++g_link.stillSteps == 300) {
            LOG("[vr] first person: camera base moved %.2f units at most in 10 s of Link standing still (height %.2f); "
                "his animations moved his eyes up to %.1f units from it (action 0x%X), not followed",
                double(g_link.stillDrift), double(g_link.stillHeight), double(g_link.heightPeak), g_link.heightPeakProc);
            g_link.stillHeight = 0;
            g_link.stillSteps = 0;
            g_link.stillDrift = 0;
            g_link.heightPeak = 0;
            g_link.heightPeakProc = 0;
        }
    }
    measure_held(link);
    place_weapon_base(link, c);
    trace_held(link, "execute");
    hammer_windup(link, step);
    // the sword's state from the game's facts of this step (its model on a hand, an attack set) and the draw asked for
    {
        static uint32_t machineLink = 0;
        if (machineLink != link) {
            machineLink = link;
            g_sword.reset();
        }
        if (step != g_link.step) {
            const bool inHand = g_held.sword && g_held.gameHand >= 0;
            const bool attacking = (ld32(link + kAtCps) & 1) != 0;
            const SwordState before = g_sword.state;
            g_sword.update(inHand, attacking, g_drawRequested, kStepSeconds);
            g_drawRequested = false;
            if (g_sword.state != before)
                LOG("[vr] sword: %s -> %s at step %llu", sword_state_name(before), sword_state_name(g_sword.state),
                    (unsigned long long)step);
            // no hit unless the sword is held for an attack (not sheathed, not the draw's own slash): the attack's
            // capsules are switched off before the game's collision check of this step (first person only)
            if (!g_sword.hits_allowed() && sword_gates() && attacking) {
                for (int i = 0; i < 3; ++i) {
                    const uint32_t cps = link + kAtCps + i * kCpsStride;
                    st32(cps, ld32(cps) & ~1u);
                }
            }
        }
    }
    trace_hand_joint(link, false);
    // (diagnostics) WWHD_VR_TRACE_SKELETON=1: every joint of his body in his own frame (forward, right, up from his
    // feet) and his game eye point, every 5 s (where the camera sits against his body: docs/VR_FIRST_PERSON_CAMERA.md)
    if (static const bool traceSkel = [] { const char* e = std::getenv("WWHD_VR_TRACE_SKELETON"); return e && *e == '1'; }();
        traceSkel && step % 150 == 0 && step != g_link.step) {
        uint16_t n = 0;
        if (const uint32_t m = matrices_of(ld32(link + kBodyModel), n)) {
            const Vec3 fwd = heading_dir(g_link.heading), right = heading_right(g_link.heading);
            auto local = [&](Vec3 p) { const Vec3 r = p - pos; return Vec3{dot(r, fwd), dot(r, right), r.y}; };
            const Vec3 e = local(ld_vec(link + kEyePos));
            LOG("[vr] skeleton: game eye point fwd %.1f right %.1f up %.1f; camera eyes fwd %.1f up %.1f", double(e.x),
                double(e.y), double(e.z), double(dot(g_link.eyeOffset, fwd)), double(g_link.eyeHeight));
            for (int j = 0; j < n; ++j) {
                const Vec3 p = local(read_joint(m, j).pos);
                LOG("[vr] skeleton: joint %d fwd %.1f right %.1f up %.1f", j, double(p.x), double(p.y), double(p.z));
            }
        }
    }
    // (diagnostics) WWHD_VR_DUMP_LINK=dir: Link's actor every 2 s, to compare states (sword sheathed, drawn)
    if (static const char* dumpDir = std::getenv("WWHD_VR_DUMP_LINK"); dumpDir && step % 60 == 0 && step != g_link.step) {
        char path[512];
        std::snprintf(path, sizeof path, "%s/link_%06llu_%s.bin", dumpDir, (unsigned long long)step,
                      g_held.sword && g_held.gameHand >= 0 ? "sword" : "none");
        if (FILE* f = std::fopen(path, "wb")) {
            std::fwrite(mem::ptr(link), 1, 0x7400, f);
            std::fclose(f);
        }
    }
    g_link.step = step;
    if (!running() || !g_head.calibrated()) return;

    // (diagnostics, 2026-11-07: "sidle took 2 attempts") the game sidles on the step A goes down only while its A action
    // is SIDLE (do status 0x10: setDoStatus, from setFrontWallType's 2: Link's collision pushed by a wall this step and
    // a line along his facing reaching it). Logged: each A press with the status then, and the status's changes to and
    // from SIDLE (at most 200 lines), with what decides it
    {
        static int lines = 0;
        static uint8_t lastStatus = 0;
        const uint32_t pl = play();
        if (pl && lines < 200) {
            const uint8_t status = ld8(pl + kPlayDoStatus);
            const bool press = (ld8(link + 0x690D) & 1) != 0;  // daPy_lk_c::mItemTrigger: doTrigger()
            const bool sidleChange = (status == 0x10) != (lastStatus == 0x10);
            if (press || sidleChange) {
                ++lines;
                LOG("[vr] sidle diag: %s at step %llu: A action %02X (was %02X), front wall type %u, wall hit %d, action 0x%X, "
                    "stick %.2f, facing %.0f (moving %.0f) degrees",
                    press ? "A pressed" : status == 0x10 ? "SIDLE offered" : "SIDLE gone", (unsigned long long)step, status,
                    lastStatus, unsigned(ld8(link + 0x68D5)), (ld32(link + 0x80C + 0x28) & 0x10) ? 1 : 0, ld32(link + kCurProc),
                    double(ldf32(link + 0x6A08)), double(s16_to_radians(int16_t(ld16(link + kShapeAngleY))) * 57.29578f),
                    double(s16_to_radians(int16_t(ld16(link + kHeading))) * 57.29578f));
            }
            lastStatus = status;
        }
    }
    // Sidle on A while its prompt was just up (docs/VR_CONTROLS.md, "Sidle"). The game offers SIDLE only on a step that
    // Link's collision was pushed by the wall in front of him (setFrontWallType's 2: ChkWallHit), and reads A on that
    // same step (checkNextAction: doTrigger with the do status SIDLE). With a GamePad the stick is held into the wall
    // while A is pressed; in first person you walk up to the wall, let go of the stick and press A, and the push, and
    // the offer with it, was gone by then: A did nothing, or took two tries (a player, 2026-11-09: "when press A to sidle
    // shows up when player presses A it should automatically put you into sidle mode"). Now A pressed within
    // kSidleGrace steps of the offer, with nothing else for A to do (do status blank), Link standing or walking where
    // it was offered, starts it as the game would: procWHideReady_init(0, &m3724), the wall's point and angle (m352C)
    // kept from that offer
    {
        constexpr uint64_t kSidleGrace = 30;     // steps (1 s)
        constexpr float kSidleStay = 25.0f;      // game units from where it was offered
        const uint32_t kProcWHideReadyInit = GC(0x023ED788);  // daPy_lk_c::procWHideReady_init(u32, cXyz*)
        constexpr uint32_t kWallPoint = 0x732C;  // m3724: the wall's point setFrontWallType found
        static uint64_t offeredStep = 0;
        static Vec3 offeredAt{};
        const uint32_t pl = play();
        if (pl) {
            const uint8_t status = ld8(pl + kPlayDoStatus);
            const uint32_t proc = ld32(link + kCurProc);
            const Vec3 at = ld_vec(link + kPos);
            if (status == 0x10) {
                offeredStep = step;
                offeredAt = at;
            } else if ((ld8(link + 0x690D) & 1) && status == 0 && offeredStep && step > offeredStep &&
                       step - offeredStep <= kSidleGrace && proc >= 4 && proc <= 6 && first_person_play() &&
                       !locked_on(link) && !carries_overhead(link)) {
                Vec3 d = at - offeredAt;
                d.y = 0;
                if (length(d) <= kSidleStay) {
                    guest_call(cpu, kProcWHideReadyInit, {link, 0, link + kWallPoint});
                    LOG("[vr] sidle: A %llu steps after its prompt (gone: Link no longer pushing the wall): started",
                        (unsigned long long)(step - offeredStep));
                    offeredStep = 0;
                }
            }
        }
    }
    // (diagnostics, the blocking audit, 2026-11-10; docs/VR_CONTROLS.md, "Blocking") the game's guard as it decides it
    // (daPy_lk_c::checkPlayerGuard: the shield-up guard pose ATNG / ATNGHAM on the upper body, the crouching guard, the
    // guard slides), each attack it blocked (mCyl's TG shield hit, daPy_lk_c::checkSuccessGuard) and each hit taken, with
    // the view, the lock-on and what held ZL (the shield raised, or the trigger)
    {
        constexpr uint32_t kUpperMove2Anim = 0x5868 + 2 * 0x10;  // m_anm_heap_upper[UPPER_MOVE2_e].mIdx (u16)
        constexpr uint32_t kCylTgHit = 0x765C + 0x98;           // mCyl.mGObjTg.mRPrm: bit 1, a shield hit
        const uint32_t proc = ld32(link + kCurProc);
        const uint16_t upper = ld16(link + kUpperMove2Anim);
        const bool guard = upper == 0x16 || upper == 0x1B || proc == 0x0C || proc == 0x0D || proc == 0x6D;
        const bool locked = locked_on(link);
        const char* view = g_thirdPerson.load() ? "third person" : "first person";
        const char* zl = g_padZL ? (g_padZLShield ? "the shield raised" : "the trigger") : "nothing";
        static bool guardWas = false;
        static int guardLines = 0;
        if (guard != guardWas && guardLines < 400) {
            ++guardLines;
            LOG("[vr] guard: %s (%s, %s; ZL held by %s, action 0x%X)", guard ? "up, blocking from the front" : "down", view,
                locked ? "locked on" : "not locked on", zl, proc);
        }
        guardWas = guard;
        static uint32_t blockedSeen = 0;
        if ((ld32(link + kCylTgHit) & 2) != 0) {
            if (blockedSeen++ < 400) LOG("[vr] guard: an attack blocked by the shield (%s, %s)", view, locked ? "locked on" : "not locked on");
        }
        static int lifeWas = -1;
        if (const uint32_t sb = save_base()) {
            const int life = ld16(sb + kSaveLife);
            if (lifeWas >= 0 && life < lifeWas)
                LOG("[vr] guard: Link hit, %d quarter hearts lost (%s, %s, guarding %s; ZL held by %s, action 0x%X)", lifeWas - life,
                    view, locked ? "locked on" : "not locked on", guard ? "yes" : "no", zl, proc);
            lifeWas = life;
        }
    }
    // parry attacks (docs/VR_CONTROLS.md, "Parry attacks"): the window and its move, a few reads a step
    {
        const uint32_t proc = ld32(link + kCurProc);
        const uint32_t pl = play();
        const bool ready = first_person_play() && ld32(link + kLockOnActor) != 0 && pl &&
                           ld8(pl + kPlayDoStatus) == kDoStatusParry;
        if (ready && !g_parry.ready) {
            LOG("[vr] parry: the window is open (locked on): A parries");
            // a full swing just before it opened: the parry's press now (kParryBuffer)
            // (any swing's start or full swing: g_parry.lastSwing)
            if (kParryBySwing && g_parry.lastSwing && step >= g_parry.lastSwing && step - g_parry.lastSwing <= kParryBuffer &&
                !(g_bParry && step < g_bTo)) {
                g_bFrom = step;
                g_bTo = step + 3;
                g_bParry = true;
                g_bThrust = g_bVertical = g_bHammer = false;
                LOG("[vr] parry: a swing %llu steps before the window opened: A now",
                    (unsigned long long)(step - g_parry.lastSwing));
            }
        }
        // The parry on its own (asked for, 2026-11-09: "when player is prompted by A the game should auto roll for the
        // player and track the enemy ... and then the game should just complete the attack on its own"): while the
        // window is open in first person and Link is not yet in a parry's move, A is pressed for him, 2 steps down and
        // 1 up, again until the move begins (the game takes A only with the enemy's attack frame past its window's
        // start: some presses did nothing, 2026-11-05). The lock-on and the view hold on the enemy through the move
        // (LockOnView), and the finishing cut is the game's own (parry_cut_wait: kParryAutoCut)
        // (2026-11-10, asked for in the headset: "parry is happening automatically, the player needs to press A first for
        // it to trigger") Only once the player has pressed A in the window (the physical A, the sword hand's primary
        // button, by its action: never the presses below): that press is latched, and A is pressed for him until the move
        // begins or the window closes
        static bool parryLatched = false, playerAWas = false;
        const bool playerA = g_input.valid && (g_input.held & (1u << int(c.leftHanded ? Action::OffPrimary : Action::MainPrimary))) != 0;  // (the right controller's A, as the boat's hop reads it)
        if (!ready || parry_move_proc(proc)) {
            parryLatched = false;
        } else if (playerA && !playerAWas && !parryLatched) {
            parryLatched = true;
            LOG("[vr] parry: A pressed in the window: the parry taken (A held for you until the move begins)");
        }
        playerAWas = playerA;
        if (parryLatched && ready && !parry_move_proc(proc) && step > g_bTo) {
            static uint64_t windowLogged = ~0ull;
            g_bFrom = step;
            g_bTo = step + 2;
            g_bParry = true;
            g_bThrust = g_bVertical = g_bHammer = g_bJump = false;
            if (windowLogged + 60 < step || windowLogged == ~0ull) {
                LOG("[vr] parry: A pressed for you after yours (the move the game's; its finishing cut your swing)");
                windowLogged = step;
            }
        }
        // the cue, the game's prompt not being shown: a pulse on the sword hand as it opens, a light one every 1/3 s
        if (ready) {
            if (g_parry.readySteps % 10 == 0)
                session().haptic(dominant_side(c), g_parry.readySteps ? c.hapticStrength * 0.5f : c.hapticStrength,
                                 g_parry.readySteps ? 0.03f : 0.08f);
            ++g_parry.readySteps;
        } else {
            g_parry.readySteps = 0;
        }
        // (diagnostics: A newly pressed in the window and no parry's move four steps on: what stood in its way.
        // changeSpecialBattle 023E5400 parries on A's press with the prompt the parry's and the enemy's attack frame
        // past its window's start; a play test's A presses in 7 of 13 windows did nothing, 2026-11-05)
        {
            static bool aWas = false;
            static uint64_t aStep = 0;
            static uint32_t aProc = 0;
            static float aNow = 0, aStart = 0;
            static unsigned aType = 0;
            const uint32_t e = ld32(link + kLockOnActor);
            if (g_padA && !aWas && ready && !parry_move_proc(proc)) {
                aStep = step;
                aProc = proc;
                aNow = e ? float(ldf32(e + kBtNowFrame)) : 0;
                aStart = e ? float(ldf32(e + kBtStartFrame)) : 0;
                aType = e ? ld8(e + kBtAttackType) : 0;
            }
            aWas = g_padA;
            if (aStep && parry_move_proc(proc)) aStep = 0;
            if (aStep && step >= aStep + 4) {
                LOG("[vr] parry: A in the window did nothing: action 0x%X then (0x%X now), the enemy's frame %.1f, its "
                    "window from %.1f, attack type %u", aProc, proc, double(aNow), double(aStart), aType);
                aStep = 0;
            }
        }
        g_parry.ready = ready;
        const bool inMove = parry_move_proc(proc);
        // (diagnostics) each sword move the game starts: what a swing became (headset tests: vertical slash counts)
        {
            static uint32_t lastMove = 0;
            const bool move = (proc >= 0x41 && proc <= 0x4A) || (proc >= 0x55 && proc <= 0x57);
            if (move && proc != lastMove) {
                static const char* const kCut[] = {"vertical slash (CUT_A)", "thrust (CUT_F)", "right cut (CUT_R)",
                                                   "left cut (CUT_L)", "finisher (CUT_EA)", "finisher (CUT_EB)",
                                                   "combo (CUT_EX_A)", "combo (CUT_EX_B)", "combo (CUT_EX_MJ)",
                                                   "combo (CUT_KESA)"};
                const char* what = proc <= 0x4A ? kCut[proc - 0x41] : proc == 0x55 ? "spin attack (CUT_TURN)"
                                   : proc == 0x56 ? "Hurricane Spin (CUT_ROLL)" : "Hurricane Spin's end (CUT_ROLL_END)";
                LOG("[vr] sword move: %s at step %llu%s", what, (unsigned long long)step,
                    locked_on(link) ? " (locked on)" : "");
                ++g_moveSerial;
            }
            lastMove = move ? proc : 0;
        }
        // a queued swing (g_queued): its press started no sword move; once Link's attack is over (no cut, sword swing,
        // spin or parry move) it is pressed again, the same move, until kQueueSteps after the swing
        if (g_queued.active) {
            const bool attacking = (proc >= 0x41 && proc <= 0x5C) || parry_move_proc(proc);
            if (g_moveSerial != g_queued.serial || inMove) {
                g_queued.active = false;  // (the press took, or a parry's move began)
            } else if (step > g_queued.until) {
                g_queued.active = false;
                LOG("[vr] swing: a queued swing dropped at step %llu (Link still busy)", (unsigned long long)step);
            } else if (step >= g_bTo && !attacking && g_sword.can_swing() && first_person_play() && !g_parry.ready) {
                g_bFrom = step;
                g_bTo = step + 2;
                g_bThrust = g_queued.thrust;
                g_bVertical = g_queued.vertical && locked_on(link);
                g_bComboReset = g_queued.comboReset;
                g_bSide = g_queued.side;
                g_bParry = g_bJump = g_bHammer = false;
                g_queued.active = false;
                LOG("[vr] swing: a queued %s pressed at step %llu, his attack over", g_bThrust ? "thrust"
                    : g_bVertical ? "downward swing (the vertical slash)" : "swing", (unsigned long long)step);
            }
        }
        if (inMove && !g_parry.inMove) {
            g_parry.moveStep = step;
            g_parry.waitLogged = false;
            LOG("[vr] parry: %s (action 0x%X)", proc == 0x5D ? "a jump over the enemy" : proc == 0x60 ? "a roll round it"
                                               : proc == 0x62 ? "a leap" : "its move", proc);
        }
        g_parry.inMove = inMove;
        // After a parry's cut (the jump's cut 0x5E and its slide 0x5F, the roll's cut 0x61, the leap's cut 0x63 and its
        // landing 0x64) Link does not carry himself away from the enemy (asked for, 2026-11-05: "don't allow him to jump
        // away, he should be in a position where he can land an attack"). Each step of those, in first-person play, his
        // own move this step is taken apart along the line from the enemy: the part away from it is dropped once he is
        // farther than kParryStayReach (flat), so he stays where he can hit it; across and towards it, his height and
        // facing stay the game's. The enemy's own steps back move only it. Written with set_link_xz (his execute
        // restores current.pos from l_debug_keep_pos: current.pos alone, the build before, only moved his drawing, the
        // view jumping between the two each step, 2026-11-05)
        {
            // (40 since 2026-11-10: a player's test, the cuts begun by his swing ended 63-70 units from the Bokoblin and
            // barely moved it, about 100 units, while the one that landed, 621 units, was at 41; 60 before)
            constexpr float kParryStayReach = 40.0f;  // (closer than this he may step out: not into the enemy)
            // (2026-11-05, a player's log: the cut over 98-113 units from the enemy every time, too far to land; asked
            // for: "when Link rolls he rolls towards the enemy, never away") From the parry's own move on (the jump
            // over 0x5D, the roll round 0x60, the leap 0x62) as well as its cut, and farther than kParryStayReach he is
            // also drawn in towards the enemy, kParryPull a step at most
            constexpr float kParryPull = 15.0f;  // (15 since 2026-11-10, with the nearer reach; 10 before)
            static bool cutting = false;
            // (diagnostics, 2026-11-10: whether the finishing cut lands) the steps its attack capsules registered a hit
            // (the hit flag, +4 bit 0, as the physical sword reads it), and his distance from the enemy as the cut began
            static int cutHits = 0;
            static float cutStartDist = -1;
            static uint32_t lastCutProc = 0;
            static Vec3 lastPos{};
            static float closest = 0, kept = 0, enemyAway = 0, pulled = 0;
            static Vec3 lastEnemy{};
            // (and the landing and fall that end it: the roll's cut lifts him and carries him about 130 units off, then
            // LAND 0x25; "Link turned 111 degrees at once (action 0x25)" and the target from 150 to 313 units in a play
            // test, 2026-11-05)
            const bool cutProc = parry_move_proc(proc) ||  // (BT_JUMP 0x5D .. BT_VERTICAL_JUMP_LAND 0x64: the move and its cut)
                                 (cutting && (proc == 0x24 || proc == 0x25 || proc == 0x27));
            // (the enemy of the cut's start: the game may let go of its target in the move)
            static uint32_t cutTarget = 0;
            const uint32_t target = cutting && cutTarget ? cutTarget : ld32(link + kLockOnActor);
            bool placed = false;
            // (first person chosen, not first_person_play: the cut's own short event, event 2 from its blow on, turns
            // the game camera off and that check with it, right as he is carried away)
            if (cutProc && target && running() && !g_thirdPerson.load()) {
                Vec3 lp = ld_vec(link + kPos);
                const Vec3 ep = ld_vec(target + kPos);
                if (finite(lp) && finite(ep)) {
                    placed = true;
                    if (!cutting) {
                        cutHits = 0;
                        cutStartDist = -1;
                        lastCutProc = 0;
                        cutting = true;
                        cutTarget = target;
                        closest = 1e9f;
                        kept = enemyAway = pulled = 0;
                        lastPos = lp;
                        lastEnemy = ep;
                    }
                    // The roll's cut (procBtRollCut, 0x61) leaps sideways round the point it was given at the roll's
                    // end (m370C), facing it, 20 units a step: that point is the middle of the roll's circle, not the
                    // enemy, so the cut carried him round and away from somewhere beside the enemy, facing past it
                    // (a player's video and log, 2026-11-09: "Link jumps away from the enemy, not towards the
                    // enemy"; his move away held back by 680-1000 units a cut). It turns about the enemy now
                    if (proc == 0x61) {
                        static uint32_t centredFor = 0;
                        Vec3 was = ld_vec(link + kRollCutCentre);
                        if (centredFor != cutTarget) {
                            centredFor = cutTarget;
                            Vec3 off = was - ep;
                            off.y = 0;
                            LOG("[vr] parry: the roll's cut turns about the enemy (its own centre was %.0f units from it)",
                                double(length(off)));
                        }
                        stf32(link + kRollCutCentre, ep.x);
                        stf32(link + kRollCutCentre + 4, was.y);
                        stf32(link + kRollCutCentre + 8, ep.z);
                    }
                    Vec3 d = lp - ep;
                    d.y = 0;
                    const float flat = length(d);
                    closest = std::min(closest, flat);
                    // (diagnostics) the cut's start (the jump's 0x5E, the roll's 0x61, the leap's 0x63) and its hits
                    const bool cutNow = proc == 0x5E || proc == 0x61 || proc == 0x63;
                    if (cutNow && lastCutProc != proc) cutStartDist = flat;
                    lastCutProc = proc;
                    if (cutNow) {
                        bool hit = false;
                        for (int i = 0; i < 3; ++i) {
                            const uint32_t cps = link + kAtCps + uint32_t(i) * kCpsStride;
                            if ((ld32(cps) & 1) && (ld32(cps + 4) & 1) && ld32(cps + 8)) hit = true;
                        }
                        if (hit) ++cutHits;
                    }
                    // (diagnostics: how far the enemy moved away by itself)
                    {
                        Vec3 em = ep - lastEnemy;
                        em.y = 0;
                        Vec3 away = lastPos - ep;
                        away.y = 0;
                        if (length(away) > 1.0f) enemyAway += std::max(0.0f, -dot(em, normalize(away)));
                    }
                    if (flat > kParryStayReach && flat > 1.0f) {
                        const Vec3 dir = d * (1.0f / flat);  // (from the enemy to him)
                        Vec3 move = lp - lastPos;
                        move.y = 0;
                        const float out = dot(move, dir);
                        if (out > 0) {
                            const Vec3 to = lp - dir * out;
                            set_link_xz(link, to.x, to.z);
                            lp.x = to.x;
                            lp.z = to.z;
                            kept += out;
                        }
                        // drawn in towards the enemy (kParryPull), not past kParryStayReach, walls checked
                        Vec3 d2 = lp - ep;
                        d2.y = 0;
                        const float flat2 = length(d2);
                        if (flat2 > kParryStayReach + 0.5f) {
                            const float want = std::min(kParryPull, flat2 - kParryStayReach);
                            // (straight at the enemy he is locked onto, stopping kParryStayReach short of it: the open
                            // ground between them, which the game's own roll crosses)
                            const Vec3 inward = d2 * (-1.0f / flat2);
                            const float go = want;
                            if (go > 0.01f) {
                                set_link_xz(link, lp.x + inward.x * go, lp.z + inward.z * go);
                                lp.x += inward.x * go;
                                lp.z += inward.z * go;
                                pulled += go;
                            }
                        }
                    }
                    lastPos = lp;
                    lastEnemy = ep;
                }
            }
            if (!placed && cutting) {
                cutting = false;
                cutTarget = 0;
                LOG("[vr] parry: the cut over, %.0f units from the enemy at the closest; his own move away %s %.0f units; "
                    "drawn in %.0f units; the enemy moved %.0f units away by itself; the cut began %.0f units from it and "
                    "hit %s (%d steps)", double(closest), kept > 0 ? "held back by" : "was", double(kept), double(pulled),
                    double(enemyAway), double(cutStartDist), cutHits ? "something" : "nothing", cutHits);
            }
        }
        // (diagnostics, the lock-on through a parry: the game's target lost or changed, with his action)
        {
            static uint32_t lastTarget = 0;
            const uint32_t target = ld32(link + kLockOnActor);
            if (target != lastTarget) {
                if (target) g_lockLostAt = 0;  // (locked again: no more presses)
                else if (lastTarget && g_padZL && first_person_play()) {
                    g_lockLostAt = std::max<uint64_t>(step, 1);
                    g_relockTries = 0;
                }
                if (lastTarget && first_person_play())
                    LOG("[vr] lock-on: the game's target %s in action 0x%X%s; ZL %s", target ? "changed" : "lost", proc,
                        inMove ? " (a parry's move)" : "",
                        g_padZL ? (g_padZLShield ? "held (by the shield)" : "held (the trigger)") : "not held");
                lastTarget = target;
            }
        }
    }

    // hits of the last collision check: haptics, once per target and swing
    const int mainHand = dominant_side(c);
    for (int i = 0; i < 3; ++i) {
        const uint32_t cps = link + kAtCps + i * kCpsStride;
        if ((ld32(cps) & 1) && (ld32(cps + 4) & 1) && g_ledger.record(ld32(cps + 8)))
            session().haptic(mainHand, c.hapticStrength, 0.06f);
    }
    // the physical sword: while the hand swings, the attack's capsules armed along the tracked blade without the
    // game's attack (first person, the sword held for an attack, no attack of the game's own)
    bool physical = false, physicalBegan = false;
    uint32_t cutType = kCutRight;
    {
        const bool gameAttack = (ld32(link + kAtCps) & 1) != 0;
        const bool allowed = c.physicalSword && cpu && sword_gates() && g_sword.hits_allowed() && g_held.sword &&
                             melee_attack(ld16(link + 0x69B0), ld32(link + kKeptEquipActor)) != 0 &&
                             g_held.gameHand >= 0 && g_input.valid && g_input.hand[mainHand].active &&
                             aspect::gameplay_camera() && !g_thirdPerson.load() &&
                             // (not in a parry's move: the swing starts the game's finishing cut, which the parried
                             // enemy cannot block; the blade's own ordinary cut was deflected every time: second player
                             // test, 2026-11-04; docs/VR_CONTROLS.md, "Parry attacks")
                             !parry_move_proc(ld32(link + kCurProc)) &&
                             // (nor locked on: the game's own moves do the cutting, each the one the swing names; the
                             // blade's ordinary cut was refused and deflected by Orca and guarding enemies: training
                             // test, 2026-11-04)
                             !locked_on(link);
        const PhysicalStep p =
            g_physical.update(allowed, gameAttack, g_swing.phase() == SwingPhase::Active, g_ledger.swing(), kStepSeconds);
        physical = p.armed;
        physicalBegan = p.began;
        if (physical) {
            const Vec3 v = rotate(yaw_quat(-g_bodyYaw), g_swing.tip_velocity());  // +X: to the body's right
            cutType = v.x >= 0 ? kCutRight : kCutLeft;
            g_physicalLink = link;
            for (int i = 0; i < 3; ++i) {
                const uint32_t cps = link + kAtCps + i * kCpsStride;
                st32(cps, ld32(cps) | 1u);
            }
        }
        if (physicalBegan) LOG("[vr] physical sword: a swing cuts (%s) at step %llu", cutType == kCutRight ? "right" : "left",
                               (unsigned long long)step);
    }
    // the game's sword attack is set (or the physical sword's): its capsules follow the tracked blade
    const uint32_t cps0 = link + kAtCps;
    if (!(ld32(cps0) & 1) || !g_input.valid || !g_input.hand[mainHand].active || !aspect::gameplay_camera() ||
        g_thirdPerson.load() ||  // third person: the game's own sword collision
        parry_move_proc(ld32(link + kCurProc))) {  // (a parry's finishing cut: the game's own, at the enemy's back)
        g_prevBladeStep = ~0ull;
        return;
    }
    const Blade xrBlade = blade_from_grip(sword_base(g_input.hand[mainHand]), c);
    uint16_t bodyCount = 0;
    const uint32_t body = matrices_of(ld32(link + kBodyModel), bodyCount);
    Blade now;
    if (g_held.sword && body && bodyCount > 12 && c.armIK && g_armWeight[mainHand] > 0.5f) {
        // the blade as it is drawn (link_draw_begin): from Link's sword hand, posed this step, along the controller,
        // with the game's own hilt and length (Hero's / Master Sword...: its reach and balance)
        const Joint hand = read_joint(body, kArmJoints[g_jointSet[mainHand]][2]);
        const Grip& grip = g_grip[g_jointSet[mainHand]];
        const Quat r = g_swordRotValid ? g_swordRot
                       : grip.valid      ? normalize(hand.rot * grip.rot)
                                         : held_sword_rotation(sword_base(g_input.hand[mainHand]), c);
        const Vec3 at = grip.valid ? hand.pos + rotate(hand.rot, grip.pos) : hand.pos;
        // (the hammer: from its head's centre to its face, so its handle and the hand do not hit)
        now.root = at + rotate(r, g_held.hammer && g_held.hammerKnown ? g_held.headLocal : g_held.baseLocal);
        now.tip = at + rotate(r, g_held.tipLocal);
    } else {
        // no posed sword hand: the controller in the tracking space at Link's eyes (a mapping of its own: g_map's
        // anchor belongs to the view)
        WorldMapping m = g_map;
        m.unitsPerMetre = world_scale();
        m.anchor = pos + g_link.eyeOffset + Vec3{0, g_link.eyeHeight + c.heightOffset * world_scale(), 0};
        now = {to_game_point(m, xrBlade.root), to_game_point(m, xrBlade.tip)};
        const float gameLength = length(ld_vec(link + kSwordTop) - ld_vec(link + kSwordRoot));
        if (std::isfinite(gameLength) && gameLength > 20.0f && gameLength < 250.0f)
            now.tip = now.root + normalize(now.tip - now.root) * gameLength;
        // the hammer: its head (the last stretch towards the face), not its handle
        if (g_held.hammer && g_held.hammerKnown)
            now.root = now.tip - normalize(now.tip - now.root) * length(g_held.tipLocal - g_held.headLocal);
    }
    if (!finite(now.root) || !finite(now.tip)) return;
    const Blade before = g_prevBladeStep != ~0ull && step <= g_prevBladeStep + 2 ? g_prevGameBlade : now;
    const Sweep s = sweep(before, now);
    for (int i = 0; i < 3; ++i) {
        const uint32_t cps = link + kAtCps + i * kCpsStride;
        st_vec(cps + kCpsGeometry, s.capsule[i].a);
        st_vec(cps + kCpsGeometry + 0xC, s.capsule[i].b);
        st_vec(cps + kAtVec, s.atVec);
    }
    // the sword's tip and root as the game's effects and sounds see them
    st_vec(link + kSwordTop, now.tip);
    st_vec(link + kSwordRoot, now.root);
    g_held.wroteTip = now.tip;
    g_held.wroteRoot = now.root;
    st_vec(link + kSwordPrevTop, before.tip);
    st_vec(link + kSwordPrevRoot, before.root);
    g_prevGameBlade = now;
    g_prevBladeStep = step;
    if (!physical) return;
    // the physical sword's attack, entered into this step's collision as Link's own attacks are (daPy_Execute does
    // so for an armed attack before this hook runs; this one was armed after it). Game code: not under the lock
    lock.unlock();
    const Cpu saved = *cpu;
    if (physicalBegan) {
        // a basic slash's power, sound and hit mark: the sword's, or a picked-up weapon's own (a stick's per its type)
        const int attack = melee_attack(ld16(link + 0x69B0), ld32(link + kKeptEquipActor));
        if (attack == 2) guest_call(cpu, kEnemyWeaponAtParam, {link, 0});
        else if (attack == 1) guest_call(cpu, kNormalCutAtParam, {link, cutType});
        else {
            *cpu = saved;  // no weapon actor: no attack, as in the game
            return;
        }
        for (int i = 0; i < 3; ++i) guest_call(cpu, kResetAtHit, {link + kAtCps + i * kCpsStride});
    }
    const uint32_t info = guest_call(cpu, kGameInfo);
    for (int i = 0; i < 3; ++i) {
        const uint32_t cps = link + kAtCps + i * kCpsStride;
        st32(cps, ld32(cps) | 1u);  // (setNormalCutAtParam keeps it; to be sure)
        guest_call(cpu, kCcSSet, {info + kCcS, cps});
        guest_call(cpu, kMassSet, {info + kMassS, cps, 1});
    }
    *cpu = saved;
}

// ---- the item wheel (docs/VR_ITEM_WHEEL.md): one update per pad read
bool wheel_allowed(const Config& c) {
    // (first person, and the Red Lion's view on the boat: vr_sail_view.h wheel_view; asked for, 2026-11-06. Free Roam
    // keeps both triggers for its zoom)
    if (!c.itemWheel || !g_head.calibrated() || !g_link.valid || !wheel_view(g_view.mode) || !aspect::gameplay_camera() ||
        subject_zoom_view() || gfx::display_gamepad_menu_open() || conducting())  // conducting: the sticks and hands play it
        return false;
    const uint32_t b = save_base();
    if (!b || ld16(b + kSaveLife) == 0) return false;  // no save in play, or Link has fallen
    const uint32_t l = g_link.link;
    return !(l && mods::camera_mouse::aiming_item(ld32(l + kCurProc)));  // aiming: the right stick aims the item
}
void show_wheel(const Config& c, bool visible) {
    if (!visible && !g_wheelShown) return;
    g_wheelShown = visible;
    WheelView v;
    v.visible = visible;
    if (visible) {
        v.pose = g_wheelPose;
        v.size = 0.42f;  // (the ring now leaves a band for the title: larger, so the ring is bigger than at 0.32)
        v.version = g_wheelVersion;
        v.pages = g_wheelPages;
        v.icons = g_wheelIcons;
        v.page = g_wheel.page();
        v.index = g_wheel.index();
        v.button = int(c.itemWheelButton);
    }
    set_wheel_view(v);
}
// the pages from the save; the selection stays on the key it was on (or, when it opens, the button's own item)
void read_wheel_pages(int button, bool opening) {
    const uint32_t b = save_base();
    std::vector<WheelPageItems> pages = b ? wheel_pages(read_wheel_save(b)) : std::vector<WheelPageItems>{};
    uint8_t keep = kNoItem;
    if (opening) {
        keep = b ? ld8(b + kSaveSelectKeys + button) : kNoItem;
    } else if (g_wheel.page() < int(g_wheelPages.size()) && g_wheel.index() < int(g_wheelPages[g_wheel.page()].entries.size())) {
        keep = g_wheelPages[g_wheel.page()].entries[g_wheel.index()].key;
    }
    auto same = [](const std::vector<WheelPageItems>& a, const std::vector<WheelPageItems>& z) {
        if (a.size() != z.size()) return false;
        for (size_t p = 0; p < a.size(); ++p) {
            if (a[p].page != z[p].page || a[p].entries.size() != z[p].entries.size()) return false;
            for (size_t i = 0; i < a[p].entries.size(); ++i) {
                const WheelEntry &x = a[p].entries[i], &y = z[p].entries[i];
                if (x.key != y.key || x.item != y.item || x.count != y.count || x.button != y.button) return false;
            }
        }
        return true;
    };
    if (!opening && same(pages, g_wheelPages)) return;
    g_wheelPages = std::move(pages);
    std::vector<int> sizes;
    int page = opening ? 0 : g_wheel.page(), index = opening ? 0 : g_wheel.index();
    for (size_t p = 0; p < g_wheelPages.size(); ++p) {
        sizes.push_back(int(g_wheelPages[p].entries.size()));
        for (size_t i = 0; i < g_wheelPages[p].entries.size(); ++i)
            if (keep != kNoItem && g_wheelPages[p].entries[i].key == keep) page = int(p), index = int(i);
    }
    g_wheel.set_pages(sizes);
    g_wheel.select(page, index);
    ++g_wheelVersion;
}
// returns whether the wheel has the right stick (and the off hand's buttons) this read
bool update_item_wheel(const Config& c, const xr::Views* head, float dt) {
    const int off = 1 - dominant_side(c);
    const auto& hand = g_input.hand[off];
    const auto& right = g_input.hand[1];  // the physical right controller's stick, whichever hand holds the sword
    WheelInput in;
    in.allowed = wheel_allowed(c);
    in.tracked = g_input.valid && hand.active && !hand.held && head;
    if (head) in.hand = rotate(conjugate(yaw_quat(g_bodyYaw)), hand.grip.position - head->head.position);
    in.side = off ? 1 : -1;
    in.swordReaching = g_reach.inside;
    in.grip = g_gripLatch[off].closed;
    in.stickX = right.active ? right.stickX : 0.0f;
    in.stickY = right.active ? right.stickY : 0.0f;
    in.cancel = right.stickClick && !g_wheelClickWas;
    g_wheelClickWas = right.stickClick;
    // the sword hand's trigger opens it while Link holds nothing (docs/VR_ITEM_WHEEL.md); it presses no button of its own
    in.trigger = (g_input.held & (1u << int(Action::MainTrigger))) != 0;
    in.triggerHand = g_input.valid && g_input.hand[dominant_side(c)].active && head;
    {
        const uint32_t l = g_link.link;
        in.emptyHanded = l && g_link.valid && game_item(l) == 0x100 /* kItemNone */ && !carries_overhead(l) &&
                         !g_reach.inside;  // (not while the hand draws the sword at its shoulder)
    }
    in.dt = dt;
    const int button = int(c.itemWheelButton);
    const int pageBefore = g_wheel.page(), indexBefore = g_wheel.index();
    const WheelEvent e = g_wheel.update(in);
    // (diagnostics, 2026-11-06: "on the boat the right trigger isn't opening the item wheel" in first person) the trigger
    // pressed while Link rides the boat and the wheel stays shut: why, once a press
    {
        static bool trigWas = false;
        const uint32_t l = g_link.link;
        if (in.trigger && !trigWas && l && g_link.valid && (ld32(play() + kPlayPlayerStatus0) & kShipRide) &&
            g_wheel.phase() == WheelPhase::Idle && !e.opened)
            LOG("[vr] item wheel on the boat: the trigger pressed, not opened (allowed %d: third person %d, play %d, menu %d, "
                "aiming %d; empty-handed %d: item 0x%X; the hand tracked %d; action 0x%X)",
                int(in.allowed), int(g_thirdPerson.load()), int(aspect::gameplay_camera()),
                int(gfx::display_gamepad_menu_open()), int(mods::camera_mouse::aiming_item(ld32(l + kCurProc))),
                int(in.emptyHanded), unsigned(game_item(l)), int(in.triggerHand), unsigned(ld32(l + kCurProc)));
        trigWas = in.trigger;
    }
    if (e.opened) {
        g_wheelIcons = wheel_icon_frames();
        read_wheel_pages(button, true);
        g_wheelReadIn = 0.25f;
        g_wheelYaw = g_bodyYaw;
        session().haptic(off, 0.35f, 0.04f);
        LOG("[vr] item wheel: opened by the %s (%zu pages; the choice goes on %s)",
            g_wheel.by_trigger() ? "sword hand's trigger" : "shield hand at its shoulder", g_wheelPages.size(),
            button == 0 ? "X" : button == 1 ? "Y" : "R");
    } else if (g_wheel.open() && (g_wheelReadIn -= dt) <= 0) {
        g_wheelReadIn = 0.25f;
        read_wheel_pages(button, false);  // the inventory changed while it is open (a pickup, a bottle filled)
    }
    if (g_wheel.open() && (g_wheel.page() != pageBefore || g_wheel.index() != indexBefore)) {
        ++g_wheelVersion;
        session().haptic(1, 0.15f, 0.02f);
    }
    if (e.committed) {
        const int p = g_wheel.page(), i = g_wheel.index();
        if (p < int(g_wheelPages.size()) && i < int(g_wheelPages[p].entries.size())) {
            const WheelEntry& chosen = g_wheelPages[p].entries[i];
            wheel_assign(button, chosen.key);
            session().haptic(off, 0.6f, 0.06f);
            // the forward pull takes the item out: its button pressed for two logic steps from the next (the game reads
            // the new item there), as a swing presses B; not for what a press would use up (wheel_presses_button)
            const bool press = wheel_presses_button(chosen.item);
            if (press) {
                const uint64_t st = interp::logic_steps();
                g_wheelPressFrom = st + 1;
                g_wheelPressTo = st + 3;
                g_wheelPressBit = button == 0 ? pad::kX : button == 1 ? pad::kY : pad::kR;
            }
            LOG("[vr] item wheel: %s (item %02X, save key %02X) put on %s%s", item_name(chosen.item), chosen.item, chosen.key,
                button == 0 ? "X" : button == 1 ? "Y" : "R", press ? " and taken out" : "");
        }
    }
    if (e.cancelled) {
        if (in.cancel) g_wheelClickUsed = true;  // the click's release is not the view toggle
        LOG("[vr] item wheel: closed without a choice (%s)", e.why);
    }
    // where it shows: ahead, a little to the off hand's side and below the eyes, turned towards the head (it was 0.42 m
    // ahead, 0.17 to the side and 0.12 below: too close to the face and off to the side, said in the headset)
    if (g_wheel.open() && head) {
        const Vec3 at = head->head.position + rotate(yaw_quat(g_wheelYaw), Vec3{0.04f * float(in.side), -0.06f, -0.65f});
        g_wheelPose.position = e.opened ? at : lerp(g_wheelPose.position, at, 1.0f - std::exp(-dt * 10.0f));
        const Vec3 z = normalize(head->head.position - g_wheelPose.position);
        const Vec3 x = normalize(cross(Vec3{0, 1, 0}, z));
        if (finite(z) && finite(x)) g_wheelPose.orientation = from_basis(x, cross(z, x), z);
    }
    show_wheel(c, g_wheel.open() && head);
    return g_wheel.consumes_stick();
}

// ---- the wrist HUD (vr_wrist_hud.h, docs/VR_WRIST_HUD.md): one update per pad read
// the hearts on top of the left wrist, the item buttons underneath it, the magic bar on top of the right wrist, the
// rupees underneath it (moved there 2026-11-09, asked for: the magic on top, as the hearts on the other side), placed
// from the controllers' grip poses (tracking space), each shown while it faces the eyes, in normal play. What they
// show is the save's: its life and maximum, its rupees, and the items the game has on X, Y and R
void update_wrist_hud(const Config& c, const xr::Views* head) {
    static bool shown[kWristPanels] = {};
    WristHudView v;
    const uint32_t b = save_base();
    // first person only: third person shows the game's own HUD (vr_ui.h)
    const bool normalPlay = running() && !g_thirdPerson.load() && b && head && g_input.valid && aspect::gameplay_camera() &&
                            !subject_zoom_view() &&
                      !gfx::display_gamepad_menu_open();
    if (b) {
        v.maxLife = ld16(b + kSaveMaxLife);
        v.life = ld16(b + kSaveLife);
        v.rupees = ld16(b + kSaveRupees);
        v.maxMagic = ld8(b + kSaveMaxMagic);
        v.magic = std::min<int>(ld8(b + kSaveMagic), v.maxMagic);
        v.testMode = test_env::active();  // (docs/VR_TEST_ENVIRONMENT.md: the rupees' panel says so)
        static std::vector<int> frames;
        if (frames.empty()) frames = wheel_icon_frames();
        for (int i = 0; i < 3; ++i) {
            v.items[i] = ld8(play() + kPlaySelectItems + i);
            v.icons[i] = v.items[i] < 256 ? frames[size_t(v.items[i])] : -1;
        }
    }
    // (the magic bar only once Link has magic: the save's maximum 0 before the first meter)
    const bool enabled[kWristPanels] = {c.armHearts, c.wristItems, c.wristRupees, c.wristMagic && v.maxMagic > 0};
    const float scale = std::clamp(c.wristHudScale, 0.5f, 2.0f);
    // metres (the picture's aspects); the hearts and rupees a quarter bigger than at first (asked for in the headset)
    const float sizes[kWristPanels][2] = {{0.17f, 0.0319f}, {0.10f, 0.034f}, {0.115f, 0.0431f},  // hearts, rupees larger again
                                          {0.17f, 0.02125f}};  // (the magic bar as wide as the hearts)
    const WristPlacement at{c.wristHudAlong, c.wristHudLift};
    for (int i = 0; i < kWristLabel; ++i) {  // (the four wrist panels; the label below)
        const int side = i == kWristRupees || i == kWristMagic ? 1 : 0;  // the physical left or right controller
        const auto& hand = g_input.hand[side];
        bool on = normalPlay && enabled[i] && hand.active;
        if (on) {
            // the hearts and magic nearer the wrist (asked for in the headset, twice: closer in): a third of the lift; the
            // panels under the wrists (the items, the rupees) at the full lift, off the palm
            const bool under = i == kWristItems || i == kWristRupees;
            const WristPlacement place = under ? at : WristPlacement{at.along, at.lift * 0.35f};
            v.pose[i] = wrist_panel_pose(hand.grip, WristPanel(i), place);
            on = wrist_panel_facing(v.pose[i], head->head.position, shown[i]);
            // the hearts and rupees straight and level, turned to the eyes (asked for in the headset: easier to read);
            // where they sit and when they show stay the wrist's
            if (i != kWristItems) {
                const Vec3 headRight = rotate(head->head.orientation, Vec3{1, 0, 0});
                v.pose[i].orientation = level_facing(v.pose[i].position, head->head.position, Vec3{headRight.x, 0, headRight.z});
            }
        }
        v.width[i] = sizes[i][0] * scale;
        v.height[i] = sizes[i][1] * scale;
        v.visible[i] = on;
        if (on != shown[i]) {
            shown[i] = on;
            static const char* const names[kWristPanels] = {"hearts (top of the left wrist)", "item buttons (under the left wrist)",
                                                            "rupees (under the right wrist)", "magic (top of the right wrist)"};
            LOG("[vr] wrist HUD: %s %s", names[i], on ? "shown" : "hidden");
        }
    }
    // the Free Roam Camera's label (vr_sail_view.h): "Free Roam" in red at the top of the view, held in the view, while
    // the Free Roam Camera is shown in normal play
    {
        // (not shown any more, asked for 2026-11-06: kFreeRoamLabel)
        const bool on = kFreeRoamLabel && running() && g_view.mode == ViewMode::FreeRoam && aspect::gameplay_camera() &&
                        !gfx::display_gamepad_menu_open();
        const LabelPlacement place;
        const WristRect& r = kWristRects[kWristLabel];
        v.visible[kWristLabel] = on;
        v.inView[kWristLabel] = true;
        v.pose[kWristLabel] = label_pose(place);
        v.width[kWristLabel] = place.width;
        v.height[kWristLabel] = place.width * float(r.h) / float(r.w);
        if (on != shown[kWristLabel]) {
            shown[kWristLabel] = on;
            LOG("[vr] free roam: its label %s", on ? "shown at the top of the view" : "hidden");
        }
    }
    set_wrist_hud(v);
}

// ---- input
void update_laser(const Config& c, const xr::Views* head);  // (the aiming laser, below)
static void filter_pad_controllers(input::PadState& s) {
    GameWorkTimer workTimer(GameWork::Pad);  // (docs/VR_PERFORMANCE.md: the VR work on the game thread)
    // the GamePad's menu state (item, pause, map, bag menus) decides when its screen shows in the headset
    gfx::display_vr_wants_gamepad_state(running());
    if (!running()) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_wheelShown) {
            g_wheel.reset();
            show_wheel(Config{}, false);
        }
        test_hammer(interp::logic_steps());  // (test aid)
        hammer_press(s, interp::logic_steps());
        return;
    }
    const Config c = config();
    std::lock_guard<std::mutex> lock(g_mutex);
    sync_input_locked(c);
    g_padReadAt = std::chrono::steady_clock::now();
    {
        // input diagnostics: a summary every 10 s, and each change of the actions held
        static auto since = std::chrono::steady_clock::now();
        static uint64_t reads = 0, valid = 0;
        static uint32_t lastHeld = ~0u;
        ++reads;
        valid += g_input.valid;
        if (g_input.valid && g_input.held != lastHeld) {
            LOG("[vr] input: actions held %04X", unsigned(g_input.held));
            lastHeld = g_input.held;
        }
        if (std::chrono::steady_clock::now() - since > std::chrono::seconds(10)) {
            LOG("[vr] input: %llu pad reads, %llu with controller input; hands %s/%s, sticks %.2f,%.2f / %.2f,%.2f; "
                "poses held through tracking gaps %u / %u reads, lost %u / %u times",
                (unsigned long long)reads, (unsigned long long)valid, g_input.hand[0].active ? "tracked" : "-",
                g_input.hand[1].active ? "tracked" : "-", double(g_input.hand[0].stickX), double(g_input.hand[0].stickY),
                double(g_input.hand[1].stickX), double(g_input.hand[1].stickY), g_trackHeld[0], g_trackHeld[1],
                g_trackLost[0], g_trackLost[1]);
            g_trackHeld[0] = g_trackHeld[1] = g_trackLost[0] = g_trackLost[1] = 0;
            LOG("[vr] input: grips at most %.2f / %.2f (a hand closes above 0.60)", double(g_gripMax[0]), double(g_gripMax[1]));
            LOG("[vr] tracking: head at most %.2f m from the view's anchor; hands beyond Link's reach %u / %u times (at "
                "most %.0f / %.0f units); positions only estimated by the runtime %u / %u reads; %u re-anchors after events",
                double(g_maxLean), g_outOfReach[0], g_outOfReach[1], double(g_maxOutOfReach[0]), double(g_maxOutOfReach[1]),
                g_estimatedReads[0], g_estimatedReads[1], g_reanchors);
            {
                const HeadStats hs = g_head.take_stats();
                if (hs.walks)
                    LOG("[vr] body under the head: %u standing steps moved, %u not all taken (%.0f of %.0f units); views "
                        "held at his back %u times", hs.walks, hs.shortWalks, double(hs.taken), double(hs.wanted), hs.backHeld);
                if (hs.floorHeld)
                    LOG("[vr] view held above the ground Link stands on %u times (the head tracked lower than his feet)",
                        hs.floorHeld);
            }
            g_maxLean = 0;
            g_outOfReach[0] = g_outOfReach[1] = g_estimatedReads[0] = g_estimatedReads[1] = g_reanchors = 0;
            g_maxOutOfReach[0] = g_maxOutOfReach[1] = 0;
            g_gripMax[0] = g_gripMax[1] = 0;
            if (g_heldPlaced) LOG("[vr] held: placed in %u draws in 10 s", g_heldPlaced);
            g_heldPlaced = 0;
            // a picked-up weapon: passes it was placed from the hand as drawn in that pass, from the pass before's hand
            // (drawn before the body), or not (frame_correct)
            if (g_lockPlaced.fresh + g_lockPlaced.carried + g_lockPlaced.none)
                LOG("[vr] sword and shield: from the drawn hand %d passes, from the last pass's carried on %d, not placed %d (10 s)",
                    g_lockPlaced.fresh, g_lockPlaced.carried, g_lockPlaced.none);
            g_lockPlaced = {};
            if (g_frameHandFar)
                LOG("[vr] hands every frame: %d hand-passes not re-aimed (the controller %.0f units from the blend at most, "
                    "over %.0f: taken for a cut) (10 s)", g_frameHandFar, double(g_frameHandFarMax), double(kFrameHandCut));
            g_frameHandFar = 0;
            g_frameHandFarMax = 0;
            if (g_weaponPlaced.fresh + g_weaponPlaced.stale + g_weaponPlaced.none)
                LOG("[vr] held weapon: in the drawn hand %d passes, from the last pass's hand %d, not placed %d (10 s)",
                    g_weaponPlaced.fresh, g_weaponPlaced.stale, g_weaponPlaced.none);
            g_weaponPlaced = {};
            since = std::chrono::steady_clock::now();
            reads = valid = 0;
        }
    }
    if (!g_input.valid) {
        if (g_wheel.phase() != WheelPhase::Idle) {
            if (g_wheel.open()) LOG("[vr] item wheel: closed without a choice (no controller input)");
            g_wheel.reset();
            show_wheel(c, false);
        }
        return;
    }
    const Bindings binds = bindings();
    const auto now = std::chrono::steady_clock::now();
    static std::chrono::steady_clock::time_point last = now;
    // the pad is read several times a frame: turning and the gesture use the time since the last read
    const float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
    last = now;
    const auto& left = g_input.hand[0];
    const auto& right = g_input.hand[1];
    // recentre: both stick clicks held (consumed: the game never sees the combination)
    const bool mainClick = g_input.held & (1u << int(Action::MainStickClick));
    const bool offClick = g_input.held & (1u << int(Action::OffStickClick));
    if (g_gesture.update(mainClick, offClick, dt)) g_head.request_recenter();
    // the camera view: a short click of the physical right stick (not part of the recentre gesture) toggles first
    // and third person in normal play; the click itself reaches the game as nothing
    {
        const bool toggled = g_viewToggle.update(right.stickClick, left.stickClick, dt);
        // not while the stick is pushed (a click while turning or moving was the third-person view by accident, seen in
        // the headset while climbing) and not while climbing (ladders, ledges, walls)
        const bool pushed = std::sqrt(right.stickX * right.stickX + right.stickY * right.stickY) > 0.5f;
        const bool climbing = g_link.link && g_link.valid && rig_follows_link_proc(ld32(g_link.link + kCurProc));
        const bool allowed = aspect::gameplay_camera() && !subject_zoom_view() && !gfx::display_gamepad_menu_open() &&
                             !pushed && !climbing;
        if (toggled && !allowed && (pushed || climbing))
            LOG("[vr] view: the right stick's click ignored (%s)", climbing ? "climbing" : "the stick pushed");
        if (toggled && allowed && !g_wheelClickUsed && !g_wheel.consumes_stick()) {
            // first and third person; while sailing on the Great Sea, then the boat's three views (vr_sail_view.h)
            const SailState sail = sail_state();
            const bool sailing = sailing_eligible(sail);
            if (sail.riding && !sailing && g_view.mode == ViewMode::ThirdPerson)
                LOG("[vr] view: the boat's views are not offered (%s)", sail_blocker(sail));
            g_view.click(sailing);
            g_budView.cancel();  // (a Baba Bud's switch back is the player's now)
            g_head.end_game_camera();  // third person looks around from where the head looks now
            g_swing = decltype(g_swing){};  // no swing measured across the switch (the hand moved meanwhile)
            view_mode_changed(g_view.mode == ViewMode::ThirdPerson ? "(the game's camera, Link's head shown)" : nullptr);
        } else {
            end_boat_view_unless_sailing();
        }
        if (!right.stickClick) g_wheelClickUsed = false;
    }
    // a Baba Bud (vr_bud_view.h, docs/VR_CONTROLS.md "Baba Buds"): from first person, third person while Link drops in
    // and is shot out; first person again when he glides on the Deku Leaf or lands. First person comes back as after an
    // event: the view anchored at the head, facing his way (head_in_game, "first person again")
    {
        const uint32_t l = g_link.link;
        const uint32_t proc = l && g_link.valid ? ld32(l + kCurProc) : 0;
        const bool eligible = l && g_link.valid && aspect::gameplay_camera() && !sailing_eligible(sail_state());
        switch (g_budView.step(proc, !g_thirdPerson.load(), eligible)) {
        case BudSwitch::ToThird:
            g_view.mode = ViewMode::ThirdPerson;
            g_head.end_game_camera();
            g_swing = decltype(g_swing){};
            view_mode_changed("(a Baba Bud: the game's camera until Link glides or lands)");
            LOG("[vr] Baba Bud: third person (action 0x%X)", proc);
            break;
        case BudSwitch::ToFirst:
            g_view.mode = ViewMode::FirstPerson;
            g_swing = decltype(g_swing){};
            view_mode_changed("(after a Baba Bud)");
            LOG("[vr] Baba Bud: first person again (action 0x%X: %s)", proc,
                proc == 0x93 ? "gliding on the Deku Leaf" : proc == kProcVomitLand || proc == 0x25 ? "landed"
                : proc == 0x26 ? "landed hard (LAND_DAMAGE)" : "out of it");
            break;
        case BudSwitch::None: break;
        }
    }
    // the boat's boomerang caught: taken out again, so it can be thrown again as on land (vr_sail_view.h
    // BoatBoomerangCatch), its item button pressed as the item wheel's choice is (g_wheelPress*). First person only
    {
        static BoatBoomerangCatch catcher;
        const uint32_t l = g_link.link;
        const bool firstOnBoat = l && g_link.valid && !g_thirdPerson.load() && aspect::gameplay_camera() &&
                                 (ld32(play() + kPlayPlayerStatus0) & kShipRide);
        if (!firstOnBoat) {
            catcher = {};
        } else if (catcher.step(ld32(l + kCurProc), ld16(l + 0x69B0))) {
            uint8_t items[3];
            for (int i = 0; i < 3; ++i) items[i] = ld8(play() + kPlaySelectItems + i);
            const int b = item_button(items, uint8_t(kItemBoomerang));
            if (b >= 0 && b <= 2) {
                const uint64_t st = interp::logic_steps();
                g_wheelPressFrom = st + 1;
                g_wheelPressTo = st + 3;
                g_wheelPressBit = b == 0 ? pad::kX : b == 1 ? pad::kY : pad::kR;
                LOG("[vr] boomerang: caught on the boat, taken out again (its button %c)", "XYR"[b]);
            }
        }
    }
    // a weapon taken out in one of the boat's views (the bow, the hookshot, the boomerang: vr_sail_view.h
    // boat_view_to_first_person): first person at once, where the motion controls aim and use it. As after a Baba Bud:
    // the view anchored at the head, facing his way
    if (const uint32_t l = g_link.link; l && g_link.valid && boat_view_to_first_person(g_view.mode, ld32(l + kCurProc))) {
        const uint32_t proc = ld32(l + kCurProc);
        g_view.mode = ViewMode::FirstPerson;
        g_swing = decltype(g_swing){};
        view_mode_changed(proc == kProcShipBow ? "(the bow taken out on the boat)"
                          : proc == kProcShipHookshot ? "(the hookshot taken out on the boat)"
                                                      : "(the boomerang taken out on the boat)");
    }
    // the head's pose this read (the item wheel, movement's forward)
    xr::Views head;
    const bool haveHead = game_views(display_time(), head);
    // the item wheel: while it is open it has the right stick and the off hand's buttons (nothing is used by accident
    // while choosing)
    const bool wheel = update_item_wheel(c, haveHead ? &head : nullptr, dt);
    update_wrist_hud(c, haveHead ? &head : nullptr);
    update_laser(c, haveHead ? &head : nullptr);
    uint32_t held = g_input.held;
    // the boat's controls (vr_sail_view.h boat_controls, docs/VR_CONTROLS.md "On the boat"): only while Link rides it
    const SailState boatState = sail_state();
    const bool boatCtl = boat_controls(boatState);
    // the right grip held on the boat, or in third person in normal play (since 2026-11-10; the left before: the left
    // guards in third person): the left stick is the D-pad (left_grip_dpad, dpad_grip_side, boat_dpad), and does not
    // steer or move Link meanwhile. Its own binding (R) is not given there
    const bool thirdPlay = aspect::gameplay_camera() && !gfx::display_gamepad_menu_open() && !subject_zoom_view() &&
                           !conducting();
    const bool gripDpad = left_grip_dpad(boatCtl, g_thirdPerson.load(), thirdPlay);
    const int dpadSide = dpad_grip_side(boatCtl);
    const bool boatDpad = gripDpad && g_gripLatch[dpadSide].closed;
    if (boatCtl) held &= ~(1u << int(c.leftHanded ? Action::OffGrip : Action::MainGrip));  // (the right grip: the boat's D-pad)
    // On the boat the left grip is a D-pad too, with the right stick (asked for, 2026-11-10: "let left grip toggle dpad
    // too then, but make it left grip + rstick dpad"; it had been R for a moment, the right trigger being R in third
    // person): held, the right stick pushed presses D-pad up, down, left or right, and the camera does not move
    // meanwhile (the stick is taken off for this read). Not with the hand at its shoulder (the item wheel's reach) or
    // the wheel up
    {
        static BoatDpad rWas = BoatDpad::None;
        bool atShoulder = false;
        if (haveHead && left.active) {
            const Vec3 rel = rotate(conjugate(yaw_quat(g_bodyYaw)), left.grip.position - head.head.position);
            atShoulder = length(rel - Vec3{-0.15f, -0.08f, 0.12f}) < 0.25f;
        }
        const bool on = boatCtl && g_gripLatch[0].closed && !atShoulder && !g_wheel.consumes_stick();
        BoatDpad d = BoatDpad::None;
        if (on) {
            d = boat_dpad(right.stickX, right.stickY);
            s.buttons |= d == BoatDpad::Up ? pad::kUp : d == BoatDpad::Down ? pad::kDown : d == BoatDpad::Left ? pad::kLeft
                         : d == BoatDpad::Right ? pad::kRight : 0u;
            g_input.hand[1].stickX = g_input.hand[1].stickY = 0;  // (the camera keeps still)
        }
        if (d != rWas) {
            static const char* const names[] = {"none", "up", "down", "left", "right"};
            LOG("[vr] boat: the left grip held, the right stick is the D-pad: %s", names[int(d)]);
            rWas = d;
        }
    }
    if (boatDpad) {
        static BoatDpad was = BoatDpad::None;
        const BoatDpad d = boat_dpad(left.stickX, left.stickY);
        s.buttons |= d == BoatDpad::Up ? pad::kUp : d == BoatDpad::Down ? pad::kDown : d == BoatDpad::Left ? pad::kLeft
                     : d == BoatDpad::Right ? pad::kRight : 0u;
        if (d != was) {
            static const char* const names[] = {"none", "up", "down", "left", "right"};
            LOG("[vr] %s: the %s grip held, the left stick is the D-pad: %s", boatCtl ? "boat" : "third person",
                dpadSide ? "right" : "left", names[int(d)]);
            was = d;
        }
    }
    if (wheel)
        held &= ~((1u << int(Action::OffPrimary)) | (1u << int(Action::OffSecondary)) | (1u << int(Action::RightStickDown)) |
                  (1u << int(Action::MainTrigger)));
    if (g_gesture.holding()) held &= ~((1u << int(Action::MainStickClick)) | (1u << int(Action::OffStickClick)));
    held &= ~(1u << int(c.leftHanded ? Action::OffStickClick : Action::MainStickClick));  // the right click: the view
    // third person: the grips are the D-pad's and the block's, not their own bindings (first person is unchanged).
    // On foot (since 2026-11-10, asked for): the right grip is the D-pad (held, the left stick picks; outside play, an
    // event or a menu, the right grip alone is D-pad up, the Wind Waker), the left grip held is the game's block, ZR
    // (Link crouches behind the shield, as the original game's R). On the boat the right grip is the D-pad too
    if (g_thirdPerson.load()) {
        const Action leftGrip = c.leftHanded ? Action::MainGrip : Action::OffGrip;
        const Action rightGrip = c.leftHanded ? Action::OffGrip : Action::MainGrip;
        held &= ~(1u << int(leftGrip));
        held &= ~(1u << int(rightGrip));  // (the right grip: the D-pad, on foot and on the boat)
        if (g_gripLatch[dpadSide].closed && !gripDpad) s.buttons |= pad::kUp;
        static bool guardWas = false;
        const bool guard = left_grip_guards(boatCtl, true, thirdPlay) && g_gripLatch[0].closed;
        if (guard) s.buttons |= pad::kZR;
        if (guard != guardWas) LOG("[vr] third person: the left grip %s", guard ? "held: the block (ZR held)" : "let go: the block ends");
        guardWas = guard;
    }
    // the Free Roam Camera (sailing only: vr_sail_view.h): the triggers zoom it and the right stick slides it, so the game
    // gets neither trigger's button nor the stick's crouch there; everywhere else they are as before
    if (g_view.mode == ViewMode::FreeRoam)
        held &= ~((1u << int(Action::MainTrigger)) | (1u << int(Action::OffTrigger)) | (1u << int(Action::RightStickDown)));
    // third person: the right trigger is R, the item on R (asked for, 2026-11-10: "make R trigger the item button on 3rd
    // person"; the right grip that was R is the D-pad there now). Not with the bow out (either trigger draws and shoots
    // it, triggers_fire_bow, below) nor in the Free Roam Camera (its zoom: the bits are taken off just above)
    if (g_thirdPerson.load()) {
        const uint32_t rightTrigger = 1u << int(c.leftHanded ? Action::OffTrigger : Action::MainTrigger);
        const uint32_t lk = g_link.link;
        const bool bowOut = lk && g_link.valid && bow_in_hand(game_item(lk));
        static bool rWas = false;
        const bool r = !bowOut && (held & rightTrigger) != 0;
        if (r) {
            s.buttons |= pad::kR;
            held &= ~rightTrigger;
        }
        if (r != rWas) LOG("[vr] third person: the right trigger %s", r ? "held: R (the item on R)" : "let go");
        rWas = r;
    }
    // the left stick's click opens the VR menu (docs/VR_MENU.md), paused; the game does not get it
    {
        const uint32_t leftClick = 1u << int(c.leftHanded ? Action::MainStickClick : Action::OffStickClick);
        const bool down = (held & leftClick) != 0;
        held &= ~leftClick;
        // (asked for here, opened at the next pass's start: menu_pause_pass. This read holds the input's lock, which
        // opening the overlay takes as well: opening it here froze the game)
        if (down && !g_menuClickHeld && !g_menuOpen.load()) g_menuRequest = true;
        g_menuClickHeld = down;
    }
    // crouch on the right stick down only in normal play: not while the right stick aims an item (bow, hookshot,
    // ...), looks through the telescope, or a GamePad menu is open (the stick does other things there)
    {
        const uint32_t l = g_link.link;
        const bool aimingItem = l && mods::camera_mouse::aiming_item(ld32(l + kCurProc));
        // (on the boat the right stick is the camera's, never ZR: the hop is A there, boat_controls; asked for, 2026-11-06)
        if (aimingItem || subject_zoom_view() || !aspect::gameplay_camera() || gfx::display_gamepad_menu_open() ||
            conducting() || boatCtl)  // (conducting: the right stick is the Wind Waker's beat)
            held &= ~(1u << int(Action::RightStickDown));
    }
    // VR archery (vr_bow.h): the off hand at the bow puts the arrow on (the game's bow button held: it draws the
    // string), either trigger shoots it (the button let go). The triggers' own buttons (A leaves the aim, ZL locks on)
    // are not given meanwhile
    {
        const uint32_t l = g_link.link;
        const uint32_t mainBit = 1u << int(Action::MainTrigger), offBit = 1u << int(Action::OffTrigger);
        const int bowSide = 1, stringSide = 0;  // the bow in the right hand either way (vr_sword.h fixed_item_side)
        BowInput bi;
        bi.aiming = l && g_link.valid && !g_thirdPerson.load() && aspect::gameplay_camera() && g_input.valid &&
                    bow_in_hand(ld16(l + 0x69B0)) && mods::camera_mouse::aiming_item(ld32(l + kCurProc));
        bi.mainTrigger = (held & mainBit) != 0;
        bi.offTrigger = (held & offBit) != 0;
        bi.stringGrip = g_gripLatch[stringSide].closed;  // (the arrow hand's grip takes the string, its release shoots)
        if (g_input.hand[bowSide].active && g_input.hand[stringSide].active)  // (the draw from the controllers themselves)
            bi.pull = length(g_input.hand[bowSide].grip.position - g_input.hand[stringSide].grip.position);
        bi.dt = dt;
        if (bi.aiming) held &= ~(mainBit | offBit);
        // the hands: the nearer pair of the drawn ones (Link's posed hand joints, metres) and the controllers (his arms
        // are shorter and reach-clamped: his hands meet before yours, measured 41 to 76 cm apart in the headset)
        bi.bowHand = g_input.hand[bowSide].grip.position;
        bi.stringHand = g_input.hand[stringSide].grip.position;
        if (!g_input.hand[bowSide].active || !g_input.hand[stringSide].active) bi.stringHand = bi.bowHand + Vec3{9, 9, 9};
        if (bi.aiming) {
            uint16_t n = 0;
            const uint32_t body = matrices_of(ld32(l + kBodyModel), n);
            if (body && n > 12 && world_scale() > 1.0f) {
                const float k = 1.0f / world_scale();
                const Vec3 bow = read_joint(body, kArmJoints[g_jointSet[bowSide]][2]).pos * k,
                           str = read_joint(body, kArmJoints[g_jointSet[stringSide]][2]).pos * k;
                if (finite(bow) && finite(str) && length(bow - str) < length(bi.bowHand - bi.stringHand)) {
                    bi.bowHand = bow;
                    bi.stringHand = str;
                }
            }
        }
        const BowOutput bo = g_bow.update(bi);
        g_bowOnBow = bo.onBow;
        // (the line stays past the shot: the arrow flies along it at its own next step, arrow_kept)
        if (bo.nocked) {
            session().haptic(bowSide, c.hapticStrength * 0.5f, 0.03f);
            session().haptic(stringSide, c.hapticStrength * 0.5f, 0.03f);
            LOG("[vr] bow: the arrow on the bow (hands %.2f m apart)", double(length(bi.bowHand - bi.stringHand)));
        }
        if (bo.drop) {  // (arrow_kept shoots it straight down at its shot: it falls from the bow)
            std::lock_guard<std::mutex> lock(g_bowLineMutex);
            g_bowDropArrow = g_bowLine.arrow;
        }
        if (bo.fire) {
            session().haptic(bi.offTrigger && !bi.mainTrigger ? stringSide : bowSide, c.hapticStrength, 0.06f);
            LOG("[vr] bow: shot (%s) at step %llu",
                bi.mainTrigger ? "the sword hand's trigger" : bi.offTrigger ? "the off hand's trigger"
                : bo.drop ? "let go before drawn back: dropped" : "the string let go",
                (unsigned long long)interp::logic_steps());
        }
    }
    // Bombs in the hand (docs/VR_HELD_ITEMS.md, "Bombs in the hand"): a bomb Link carries is held in the sword hand
    // (link_executed places it there each step), and letting go of that hand's grip after squeezing it throws it,
    // with the hand's own motion (the bomb's button pressed for the game's throw, its speed then set from the hand's
    // velocity: link_executed). The hand's velocity is taken over the last kThrowWindow seconds of its places
    {
        const uint32_t l = g_link.link;
        const int d = dominant_side(c);
        const xr::HandState& hd = g_input.hand[d];
        g_handClock += dt > 0 && dt < 1.0f ? dt : 0.0f;
        if (g_input.valid && hd.active) {
            g_handTrail[g_handTrailN % kHandTrail] = {hd.grip.position, g_handClock};
            ++g_handTrailN;
        }
        Vec3 vel{};
        if (g_handTrailN >= 2) {
            const HandSample& now = g_handTrail[(g_handTrailN - 1) % kHandTrail];
            for (int back = 1; back < kHandTrail && back < g_handTrailN; ++back) {
                const HandSample& then = g_handTrail[(g_handTrailN - 1 - back) % kHandTrail];
                if (now.t - then.t >= kThrowWindow || back == kHandTrail - 1 || back == g_handTrailN - 1) {
                    if (now.t - then.t > 1e-3f) vel = (now.pos - then.pos) * (1.0f / (now.t - then.t));
                    break;
                }
            }
        }
        const bool bomb = l && g_link.valid && !g_thirdPerson.load() && first_person_play() && carried_bomb(l);
        static bool gripWas = false, armed = false;
        const bool grip = g_gripLatch[d].closed;
        // (its grip holds the bomb: not also the game's button it presses, R, which throws what Link carries)
        if (bomb) held &= ~(1u << int(Action::MainGrip));
        if (bomb && grip != gripWas)
            LOG("[vr] bomb: the sword hand's grip %s (controller %d, value %.2f)", grip ? "squeezed: held" : "opened",
                d, double(hd.grip_value));
        // a throwing motion of the hand (asked for: "use a throw motion with their hands") throws it too: faster than
        // kThrowSpeed, at least kThrowAcross of it not straight up or down (so lifting and lowering it does not)
        const float across = std::sqrt(vel.x * vel.x + vel.z * vel.z);
        const bool throwing = g_bombThrow.pending && interp::logic_steps() - g_bombThrow.step < 30;  // (one already asked)
        const bool motion = bomb && length(vel) > kThrowSpeed && across > kThrowAcross && !throwing;
        if (motion) LOG("[vr] bomb: a throwing motion of the hand (%.2f m/s)", double(length(vel)));
        if (!bomb) armed = false;
        else if (grip && !motion) armed = true;
        else if ((armed && gripWas) || motion) {
            armed = false;
            uint8_t items[3];
            for (int i = 0; i < 3; ++i) items[i] = ld8(play() + kPlaySelectItems + i);
            const int b = item_button(items, kItemBombBag);
            s.buttons |= b == 0 ? pad::kX : b == 1 ? pad::kY : b == 2 ? pad::kR : pad::kA;
            g_bombThrow = {true, carried_bomb(l), to_game_dir(g_map, vel) * world_scale(), interp::logic_steps()};
            LOG("[vr] bomb: let go to throw it (hand %.2f m/s)", double(length(vel)));
        }
        gripWas = grip;
        // Anything else he carries over his head (a pot, a barrel, a rock, a seed: whatever the A button throws;
        // asked for, 2026-11-06: "the same throw motion from the bombs to throw the item"): the bombs' throwing motion
        // of the sword hand presses A for two logic steps, as the player's press would (the game throws on a fresh A,
        // checkNextActionGrab). Only while A reads "Throw" (the do status 0xE), never its drop or a door's open; the
        // game's own throw and arc. One at most every kCarryThrowGap steps
        const bool carried = l && g_link.valid && !g_thirdPerson.load() && first_person_play() && carries_overhead(l) &&
                             !carried_bomb(l);
        static uint64_t carryThrowFrom = 0, carryThrowTo = 0;
        const uint64_t st = interp::logic_steps();
        if (carried && st >= carryThrowTo + kCarryThrowGap && length(vel) > kThrowSpeed && across > kThrowAcross) {
            const uint32_t pl = play();
            if (pl && ld8(pl + kPlayDoStatus) == kDoStatusThrow) {
                carryThrowFrom = st;
                carryThrowTo = st + 2;
                LOG("[vr] carried: thrown by a throwing motion of the hand (%.2f m/s) at step %llu", double(length(vel)),
                    (unsigned long long)st);
            }
        }
        if (st >= carryThrowFrom && st < carryThrowTo) s.buttons |= pad::kA;
    }
    // an aimed item (bow, hookshot, boomerang, grappling hook): its trigger uses it, pressing and holding the item's own
    // button as long as the trigger is held (asked for in the headset: the right trigger shoots the bow, the left
    // launches the grappling hook). That trigger's own button is not given meanwhile (A leaves the aim, ZL locks on)
    // The boomerang also while it is only held (not aimed): caught again, Link holds it without aiming, and the game
    // needs its button to aim and throw it (seen in the headset: after the catch it could not be thrown again); one press
    // throws it (the game throws on the release). The grappling hook as well while held standing, walking or locked on
    // (actions 4 to 9): after one use Link holds it without aiming, and it could not be fired again (reported in the headset);
    // until it is put away or another item is taken out
    {
        const uint32_t l = g_link.link;
        const uint32_t proc = l ? ld32(l + kCurProc) : 0;
        const uint16_t heldItem = l ? ld16(l + 0x69B0) : 0;
        if (l && g_link.valid && !g_thirdPerson.load() && aspect::gameplay_camera() &&
            (mods::camera_mouse::aiming_item(proc) || heldItem == kItemBoomerang ||
             heldItem == kItemDekuLeaf ||  // (the leaf held: the left trigger is its button, not Z-targeting)
             (heldItem == kItemGrapplingHook && proc >= 4 && proc <= 9))) {  // (8, 9: locked on, the right trigger)
            const uint16_t item = ld16(l + 0x69B0);
            // (the physical left trigger is the sword hand's in left-handed play: Action::MainTrigger)
            const Action leftTrigger = c.leftHanded ? Action::MainTrigger : Action::OffTrigger;
            const uint32_t trigger = fired_by_both_triggers(item)
                                         ? (1u << int(Action::MainTrigger)) | (1u << int(Action::OffTrigger))
                                     : fired_by_left_trigger(item)
                                         ? 1u << int(leftTrigger)
                                         : 1u << int(fired_by_off_trigger(item) ? Action::OffTrigger : Action::MainTrigger);
            uint8_t items[3];
            for (int i = 0; i < 3; ++i) items[i] = ld8(play() + kPlaySelectItems + i);
            const int b = item_button(items, uint8_t(item));
            const uint32_t button = b == 0 ? pad::kX : b == 1 ? pad::kY : pad::kR;
            if (b >= 0 && fired_by_trigger(item)) {
                if (held & trigger) s.buttons |= button;
                held &= ~trigger;
            }
            // the grappling hook: the physical right trigger locks on (ZL), the left trigger, its own button, having
            // taken the lock-on's place (asked for in the headset)
            if (item == kItemGrapplingHook) {
                const uint32_t rightTrigger = 1u << int(c.leftHanded ? Action::OffTrigger : Action::MainTrigger);
                if (held & rightTrigger) s.buttons |= pad::kZL;
                held &= ~rightTrigger;
            }
            // the boomerang thrown by a downward swing of its hand (above)
            if (b >= 0 && (item == kItemBoomerang || item == kItemDekuLeaf)) {  // (the leaf: a swing up or down)
                const uint64_t st = interp::logic_steps();
                if (st >= g_throwFrom && st < g_throwTo) s.buttons |= button;
            }
            // the bow: its button held for the player while the arrow is on the bow (vr_bow.h): drawn; let go: shot
            if (b >= 0 && bow_in_hand(item) && g_bowOnBow) s.buttons |= button;
        }
    }
    // third person with the bow out: either trigger draws it (its item button held) and shoots it when let go
    // (vr_sword.h triggers_fire_bow), on land and on the boat
    {
        const uint32_t l = g_link.link;
        const uint16_t item = l && g_link.valid ? ld16(l + 0x69B0) : 0;
        if (l && g_link.valid &&
            triggers_fire_bow(g_thirdPerson.load(), aspect::gameplay_camera() && !gfx::display_gamepad_menu_open(), item)) {
            const uint32_t both = (1u << int(Action::MainTrigger)) | (1u << int(Action::OffTrigger));
            uint8_t items[3];
            for (int i = 0; i < 3; ++i) items[i] = ld8(play() + kPlaySelectItems + i);
            const int b = item_button(items, uint8_t(item));
            static bool drawWas = false;
            const bool draw = b >= 0 && b <= 2 && (held & both) != 0;
            if (draw) s.buttons |= b == 0 ? pad::kX : b == 1 ? pad::kY : pad::kR;
            if (draw != drawWas)
                LOG("[vr] bow (third person): %s by the trigger", draw ? "drawn" : "let go: shot");
            drawWas = draw;
            held &= ~both;
        }
    }
    // conducting: the GamePad screen's song list hidden, the menu button shows or hides it (asked for in the headset);
    // the press is not the game's Plus then
    {
        static bool menuWas = false, panel = false, wasConducting = false;
        const bool now = conducting();
        if (now != wasConducting) panel = false;  // hidden as each conducting begins
        wasConducting = now;
        const uint32_t menuBit = 1u << int(Action::Menu);
        const bool menu = (held & menuBit) != 0;
        if (now) {
            if (menu && !menuWas) {
                panel = !panel;
                LOG("[vr] Wind Waker: the GamePad screen %s (menu button)", panel ? "shown" : "hidden");
            }
            held &= ~menuBit;
        }
        menuWas = menu;
        gfx::display_vr_baton_panel(panel);
    }
    s.buttons |= binds.buttons(held);
    // sailing the boat the controller's A (the right controller's primary button) also presses ZR: the boat's hop
    // (boat_hop: not with the cannon or the salvage arm out); its own press stays the game's
    {
        const uint32_t aBit = 1u << int(c.leftHanded ? Action::OffPrimary : Action::MainPrimary);
        static bool hopWas = false;
        const bool hop = boat_hop(boatState) && (held & aBit) != 0;  // (sailing only: not the cannon, the salvage arm)
        if (hop) s.buttons |= pad::kZR;
        if (hop && !hopWas) LOG("[vr] boat: A pressed: the hop (ZR)");
        hopWas = hop;
    }
    const uint64_t step = interp::logic_steps();
    test_hammer(step);  // (test aid)
    if (step >= g_bFrom && step < g_bTo) {
        // the sword's button; A for a picked-up weapon (B throws it: vr_sword.h swing_presses_do_button); the
        // hammer's own item button (hammer_press)
        // (a swing made with the hammer never presses B, whatever Link holds by the time the press lands: B with the hammer
        // out puts it away and cuts with the sword, seen in the headset; it presses the hammer's own button, hammer_press)
        const uint32_t l = g_link.link;
        const uint16_t item = l && g_link.valid ? game_item(l) : 0;
        if (!g_bHammer && item != kItemSkullHammer && !(g_held.hammer && g_held.gameHand >= 0))
            s.buttons |= swing_presses_do_button(item) || g_bParry || g_bJump ? pad::kA : pad::kB;
        // (a thrust or vertical slash locked on: a new combo, its basic move every time; written before the game reads
        // the press, on the game thread)
        if (g_bComboReset && step == g_bFrom && l && g_link.valid) st8(l + kComboCount, 0);
    }
    if (step >= g_wheelPressFrom && step < g_wheelPressTo) s.buttons |= g_wheelPressBit;  // the item wheel's choice

    // conducting with the Wind Waker (vr_baton.h, docs/VR_WIND_WAKER.md): the left stick or the wand's hand holds the
    // note, the right stick or the other hand the beat, given to the game's two sticks; neither moves Link nor turns the view meanwhile
    BatonOutput baton;
    {
        BatonInput bi;
        bi.conducting = conducting();
        for (int h = 0; h < 2; ++h) {
            bi.stick[h][0] = g_input.hand[h].stickX;
            bi.stick[h][1] = g_input.hand[h].stickY;
            bi.tracked[h] = g_input.hand[h].active;
            bi.hand[h] = g_input.hand[h].grip.position;
            // the notes' hand (the wand's: the sword hand) read at the wand's tip, kBatonTip ahead along the controller's
            // aim: a flick of the wrist moves it too, not only a reach of the arm (asked for, 2026-11-09: "you need to
            // stretch your hand out too far"). About 9 degrees of wrist turns it 3 cm, the right hand's outward note
            // (0.08 since 2026-11-10, calibrated in the headset: "holding your arm in the middle triggers the left"; with
            // the arm still, a relaxed turn of the wrist swung the point 20 cm out about 9 cm, a direction at the 9 cm
            // zones. 8 cm out: 25 degrees of wrist moves it 3.4 cm, inside the middle)
            constexpr float kBatonTip = 0.08f;
            if (h == dominant_side(c))
                bi.hand[h] = bi.hand[h] + rotate(g_input.hand[h].aim.orientation, Vec3{0, 0, -kBatonTip});
        }
        bi.tracked[0] = bi.tracked[0] && haveHead;  // (the centre's frame is the head's heading)
        bi.tracked[1] = bi.tracked[1] && haveHead;
        bi.headingYaw = haveHead ? yaw_of(head.head.orientation) : 0.0f;
        bi.hands = c.batonHands;
        bi.reach = c.batonHandReach;
        bi.noteHand = dominant_side(c);  // (the wand's hand: read at the wand's tip, with the wand's zone)
        // its channel the right stick, the other hand's the left (asked for in the headset, 2026-11-09: "the left hand and
        // right hand needs their roles reversed, right hand should be the one hitting the notes", the calibrations kept)
        bi.swapHands = true;
        // the wand's hand's trigger holds the middle note (asked for in the headset, 2026-11-10: "still too hard for
        // middle note"); the trigger presses nothing while conducting (it opens the item wheel, which cannot open then)
        bi.middle[dominant_side(c)] = g_input.hand[dominant_side(c)].active && g_input.hand[dominant_side(c)].trigger > 0.5f;
        {
            static bool middleWas = false;
            const bool m = bi.middle[dominant_side(c)] && conducting();
            if (m != middleWas) LOG("[vr] Wind Waker: the middle %s by the trigger", m ? "held" : "let go");
            middleWas = m;
        }
        bi.stickEnter = std::max(0.5f, c.stickDeadZone + 0.2f);
        bi.stickExit = std::max(0.35f, c.stickDeadZone + 0.05f);
        bi.recentre = g_head.recenter_requested();
        bi.dt = dt;
        baton = g_baton.update(bi);
        if (baton.started || baton.stopped)
            LOG("[vr] Wind Waker: %s", baton.started ? (dominant_side(c) == 1
                                                            ? "conducting (the right hand with the right stick, the left hand with the left stick)"
                                                            : "conducting (the left hand with the right stick, the right hand with the left stick)")
                                                     : "conducting ended");
        static BatonDir logged[2] = {};
        for (int ch = 0; ch < 2; ++ch)
            if (baton.dir[ch] != logged[ch]) {
                logged[ch] = baton.dir[ch];
                if (g_baton.conducting())
                    LOG("[vr] Wind Waker: %s %s (stick %s, hand %s%s; the %s hand %.1f cm across, %.1f up)",
                        ch ? "right stick (notes)" : "left stick (beat)", baton_dir_name(baton.dir[ch]),
                        baton_dir_name(baton.stickDir[ch]), baton_dir_name(baton.handDir[ch]),
                        g_baton.hand_flicking(ch) ? ", a flick" : "", g_baton.hand_of(ch) ? "right" : "left",
                        double(g_baton.hand_offset(ch).x * 100.0f), double(g_baton.hand_offset(ch).y * 100.0f));
            }
        // (diagnostics) the middle taken again where a hand rested in a direction (BatonHand::kRestTime)
        {
            static uint32_t restsSeen[2] = {};
            for (int ch = 0; ch < 2; ++ch) {
                const uint32_t r = g_baton.hand_rests(ch);
                if (r != restsSeen[ch]) {
                    restsSeen[ch] = r;
                    LOG("[vr] Wind Waker: the %s hand rested in a direction: its middle taken there", g_baton.hand_of(ch) ? "right" : "left");
                }
            }
        }
        // (the calibration, 2026-11-10) while conducting, each hand's farthest reach from its centre each way, once a
        // second: how far you move for each direction, against the zones (BatonZone, kNoteReachShare, kNoteShape)
        if (g_baton.conducting()) {
            static float most[2][4] = {};  // per controller: left, right, up, down (cm)
            static auto lastLog = std::chrono::steady_clock::now();
            for (int ch = 0; ch < 2; ++ch) {
                if (!g_baton.hand_settled(ch)) continue;
                const int h = g_baton.hand_of(ch);
                const Vec3 o = g_baton.hand_offset(ch) * 100.0f;
                most[h][0] = std::max(most[h][0], -o.x);
                most[h][1] = std::max(most[h][1], o.x);
                most[h][2] = std::max(most[h][2], o.y);
                most[h][3] = std::max(most[h][3], -o.y);
            }
            if (const auto now = std::chrono::steady_clock::now(); now - lastLog > std::chrono::seconds(1)) {
                lastLog = now;
                LOG("[vr] Wind Waker reach (cm, left / right / up / down): left hand %.1f %.1f %.1f %.1f, right hand %.1f %.1f %.1f %.1f; "
                    "the notes' zone %.1f cm (grows with the moves)",
                    double(most[0][0]), double(most[0][1]), double(most[0][2]), double(most[0][3]), double(most[1][0]),
                    double(most[1][1]), double(most[1][2]), double(most[1][3]), double(g_baton.hand_radius(1) * 100.0f));
                for (auto& m : most) for (float& v : m) v = 0;
            }
        }
    }

    // left stick: movement through the game's own movement code; "forward" is the head's (or the left hand's)
    // heading, which dCam_getControledAngleY returns while Link walks (controlled_angle_hook)
    if (g_head.calibrated()) {
        if (c.moveDirection == MoveDirection::LeftHand && left.active) {
            g_wantedHeading = g_map.heading + yaw_of(left.aim.orientation);
            g_haveWanted = true;
        } else if (haveHead) {
            // with Detached Arms: the head's heading turned towards where the hands reach, as the standing body follows
            // (body_follow_yaw). Moving, the game turns Link to this heading: by the head alone, a look round or a
            // reach while he moved left his body facing away from the arms until he stood again (reported in the
            // headset)
            float facing = yaw_of(head.head.orientation);
            if (c.detachedArms && (left.active || right.active)) {
                const Vec3 mid = left.active && right.active ? (left.grip.position + right.grip.position) * 0.5f
                                 : left.active ? left.grip.position : right.grip.position;
                facing = body_follow_yaw(facing, true, mid - head.head.position);
            }
            g_wantedHeading = g_map.heading + facing;
            g_haveWanted = true;
        }
    }
    if (g_baton.conducting()) {
        float x = 0, y = 0;
        baton_stick(baton.dir[0], x, y);  // the note: a full push its way (not scaled by vr.moveSpeed)
        g_stickPushed = false;
        g_stickBack = false;
        s.lx = std::clamp(s.lx + x, -1.0f, 1.0f);
        s.ly = std::clamp(s.ly + y, -1.0f, 1.0f);
    } else {
        // (on the boat with the left grip held the stick is the D-pad: no movement, no steering)
        const Stick move = boatDpad ? Stick{} : dead_zone({left.stickX, left.stickY}, c.stickDeadZone);
        g_stickPushed = move.x != 0 || move.y != 0;
        const float stickAngle = std::fabs(std::atan2(move.x, move.y));
        g_stickBack = g_stickPushed && stickAngle > 100.0f * kPi / 180.0f;
        // aiming an item in first person: the game walks Link only near a full push (measured in the headset: 0.46
        // and less, still; 1.0, 14.4 a step), so a push past the dead zone counts from kAimStickFloor up (asked for:
        // move while aiming, as another VR game does)
        Stick moveIn = move;
        // (not on the boat: Link sits there, placed by the boat each step, and the stick is the boat's)
        if (const uint32_t al = g_link.link; al && g_link.valid && g_stickPushed && first_person_play() &&
                                             !boatState.riding && mods::camera_mouse::aiming_item(ld32(al + kCurProc))) {
            const float m = std::sqrt(move.x * move.x + move.y * move.y);
            if (m > 1e-3f) {
                const float k = std::min(1.0f, kAimStickFloor + (1.0f - kAimStickFloor) * std::min(m, 1.0f)) / m;
                moveIn = {move.x * k, move.y * k};
            }
        }
        s.lx = std::clamp(s.lx + moveIn.x * c.moveSpeed, -1.0f, 1.0f);
        s.ly = std::clamp(s.ly + moveIn.y * c.moveSpeed, -1.0f, 1.0f);
        // (diagnostics) moving while aiming an item: the game moves Link with the left stick in its first-person aims
        // (HD: setSpeedAndAngleAtn, lk_aimSpeedBoost), yet he did not move in the headset (reported with a video).
        // Once a second while aiming with the stick pushed: what the game is given and how fast he goes
        if (const uint32_t al = g_link.link; al && g_link.valid && g_stickPushed &&
                                             mods::camera_mouse::aiming_item(ld32(al + kCurProc))) {
            static auto lastAim = std::chrono::steady_clock::time_point{};
            if (const auto now = std::chrono::steady_clock::now(); now - lastAim > std::chrono::seconds(1)) {
                lastAim = now;
                LOG("[vr] aiming and moving: action 0x%X, stick to the game %.2f %.2f, his speed %.2f, heading %d",
                    unsigned(ld32(al + kCurProc)), double(s.lx), double(s.ly), double(float(ldf32(al + kSpeedF))),
                    int(int16_t(ld16(al + kHeading))));
            }
        }
        // walking backwards (asked for in the headset): the game turned him round to walk that way, his model and arms
        // then facing away from the view; its own back step (Z with no target) went wrong at its start and end (he
        // snapped round, or never began it). In VR he keeps standing (4, 5) instead, so his model faces where your body
        // faces every step (link_executing) and stays in step with you, and VR moves him backwards itself
        // (back_slide): the game gets no stick meanwhile, so it never turns him; ZL and lock-on stay the game's.
        // From the stick pointing back of 100 degrees until it points forward of 80 or is let go; walking (6) he stops
        // first. Not while locked on, on a ladder, swimming or anything else: the game's movement there
        const uint32_t l = g_link.link;
        const uint32_t proc = l && g_link.valid ? ld32(l + kCurProc) : 0;
        const bool was = g_backStep;
        const bool back = g_stickPushed && stickAngle > (was ? 80.0f : 100.0f) * kPi / 180.0f;
        const bool step = back && haveHead && g_head.calibrated() && first_person_play() && proc >= 4 && proc <= 6;
        if (step != was) {
            g_backStep = step;
            LOG("[vr] walking backwards: %s (action 0x%X)",
                step ? "Link stands facing the way you face and is moved back" : "ends", proc);
        }
        g_backSlide = {};
        // (not on the boat: seen in the player's log, 2026-11-06, "moving while aiming: Link moved by the stick (action
        // 0x8D)" with the bow at the tiller; VR's slide would move him off his seat)
        const bool aimStep = !step && g_stickPushed && haveHead && g_head.calibrated() && first_person_play() &&
                             !boatState.riding && mods::camera_mouse::aiming_item(proc);
        if (aimStep != g_aimStep) {
            g_aimStep = aimStep;
            LOG("[vr] moving while aiming: %s (action 0x%X)", aimStep ? "Link moved by the stick" : "ends", proc);
        }
        if (aimStep) {
            g_stickBack = false;
            s.lx = s.ly = 0;  // (moved in back_slide; the game's aim keeps facing where the hand aims)
            g_backSlide = rotate(yaw_quat(yaw_of(head.head.orientation)), Vec3{move.x, 0, -move.y}) * c.moveSpeed;
        }
        if (step) {
            g_stickBack = false;
            s.lx = s.ly = 0;  // (the stick moved in back_slide: the game would turn him round to it)
            // the stick from where the head looks (tracking space), scaled as the game's stick is
            g_backSlide = rotate(yaw_quat(yaw_of(head.head.orientation)), Vec3{move.x, 0, -move.y}) * c.moveSpeed;
            // (diagnostics) once a second: his model's facing against the body's (0 wanted: in step with you)
            static auto lastLog = std::chrono::steady_clock::time_point{};
            if (const auto now = std::chrono::steady_clock::now(); now - lastLog > std::chrono::seconds(1)) {
                lastLog = now;
                const float off = wrap_radians(s16_to_radians(int16_t(ld16(l + kShapeAngleY))) -
                                               link_facing(g_map.heading, g_body.yaw));
                LOG("[vr] walking backwards: action 0x%X, his facing %.1f degrees from yours, arms %.2f / %.2f", proc,
                    double(off * 180.0f / kPi), double(g_armWeight[0]), double(g_armWeight[1]));
            }
        }
    }
    hammer_press(s, step);  // (after the movement: a side swing's stick is the whole stick for its two steps)
    thrust_press(s, step);  // (likewise: a thrust's forward stick)
    vertical_press(s, step);  // (and a downward swing's middle stick, locked on: the vertical slash)
    // The shield hand lowered and raised (asked for, 2026-11-04; docs/VR_CONTROLS.md, "Shield down, shield up"): the hand
    // down by the hip (kShieldDown under the head) lowers the shield and lets go of a lock-on this started; brought up
    // again in front of the body (above kShieldUp, within kShieldUpSide of the middle) it raises the shield and locks
    // onto the nearest enemy: ZL held while the hand stays up, the game choosing the target, and only when it has one
    // to lock onto (dAttention_c mLockonCount), so a raise with nothing near never starts the target-less Z walk. Ends
    // when the lock does (the enemy gone) or none came within kShieldLockWait steps
    {
        constexpr float kShieldDown = 0.52f, kShieldUp = 0.47f, kShieldUpSide = 0.45f;
        constexpr uint64_t kShieldLockWait = 10;
        static bool down = false, holding = false, lockSeen = false, haveUpRef = false;
        static uint64_t heldFrom = 0, lockSeenAt = 0;
        // (asked for, 2026-11-04: "it should stay locked on until player lowers their arm 5cm from the position it first
        // locked in on") the hand's height when the lock came (or at the raise until then); lowered kShieldUnlockDrop
        // under it, the shield is down and the lock let go
        static float upRef = 0;
        constexpr float kShieldUnlockDrop = 0.03f;  // (3 cm, the user's choice after the play tests; 5 at first)
        // (the locked target farther than this, game units flat: the lock let go; a first guess, about 7 m, tuned from
        // the distance lines in the log)
        constexpr float kShieldLockMaxDistance = 500.0f;  // (700 at first; 500 asked for after the play tests)
        constexpr float kShieldLockFrontDegrees = 70.0f;
        // (raised again: kShieldRaiseAbove over where it was lowered. The fixed height before, 0.47 m under the head,
        // sat 1 cm from the lowering 3 cm under the lock's and the shield flickered down and up: play test, 2026-11-04)
        constexpr float kShieldRaiseAbove = 0.06f;
        static float downAt = -1.0f;  // (the target this far either side of where the head looks)  // (5 cm at first; 3 asked for after the play test)
        const int off = 1 - dominant_side(c);
        const uint32_t lk = g_link.link && g_link.valid ? g_link.link : 0;
        const bool usable = lk && haveHead && g_head.calibrated() && first_person_play() && g_input.valid &&
                            g_input.hand[off].active && g_held.shieldOnHand;
        if (!usable) {
            down = holding = haveUpRef = false;
        } else {
            const Vec3 rel = rotate(conjugate(yaw_quat(g_bodyYaw)), g_input.hand[off].grip.position - head.head.position);
            // the motion, not fixed heights (a play test: only 5 of many lowers and raises reached 0.55 m down and back
            // to 0.40 m): the hand's height over the last kShieldWindow steps; down: kShieldTravel under its highest
            // there and below the chest, up: kShieldTravel over its lowest and back about the chest, in front
            constexpr int kShieldWindow = 24;  // (0.8 s)
            // (easier, asked for after the play test, 2026-11-04: 0.18 m of travel, 0.30 / 0.50 m and 0.40 m to the side
            // before)
            constexpr float kShieldTravel = 0.12f, kShieldDownBelow = 0.45f, kShieldUpAbove = 0.50f, kShieldSide = 0.50f;
            static float heights[kShieldWindow] = {};
            static int heightCount = 0, heightAt = 0;
            heights[heightAt] = rel.y;
            heightAt = (heightAt + 1) % kShieldWindow;
            heightCount = std::min(heightCount + 1, kShieldWindow);
            float highest = rel.y, lowest = rel.y;
            for (int i = 0; i < heightCount; ++i) {
                highest = std::max(highest, heights[i]);
                lowest = std::min(lowest, heights[i]);
            }
            // (diagnostics: the shield hand in the body's frame whenever it moved 5 cm, the reference for tuning)
            {
                static Vec3 lastLogged{1e9f, 0, 0};
                if (length(rel - lastLogged) > 0.05f) {
                    lastLogged = rel;
                    LOG("[vr] shield hand: %.2f across, %.2f from the head, %.2f forward (%s)", double(rel.x),
                        double(rel.y), double(-rel.z), down ? "down" : "up");
                }
            }
            // (the play test's own heights, 2026-11-04: the hand lowered at 0.53 to 0.71 m under the head and raised at
            // 0.32 to 0.45 m; the first motion rules flickered, a raise at once taken back by the window's height from
            // before the lowering. The window starts again at each change, and a change holds kShieldHold reads)
            constexpr int kShieldHold = 6;
            static int sinceChange = 0;
            ++sinceChange;
            const bool droppedNow = highest - rel.y >= kShieldTravel && rel.y < -kShieldDownBelow;
            const bool raisedNow = rel.y - lowest >= kShieldTravel && rel.y > -kShieldUpAbove &&
                                   std::fabs(rel.x) < kShieldSide && -rel.z > -0.10f;
            const bool wasDown = down;
            if (sinceChange < kShieldHold) {
                // (held: no change yet)
            } else if (!down && (haveUpRef ? rel.y < upRef - kShieldUnlockDrop : (droppedNow || rel.y < -kShieldDown))) {
                down = true;
                downAt = rel.y;
                if (holding) LOG("[vr] shield: lowered; the lock-on it held is let go");
                else LOG("[vr] shield: lowered");
                holding = false;
            } else if (down && (raisedNow || (rel.y > downAt + kShieldRaiseAbove && std::fabs(rel.x) < kShieldUpSide &&
                                              -rel.z > 0.05f))) {
                down = false;
                // (ZL held whatever the attention list says: its count, read at play + 0x4564 + 0xD4, was 0 at every
                // raise of a play test while the trigger locked onto the enemy beside it, and nothing happened; with no
                // target the game takes Z for its target-less walk, procAtnMove 7, and the hold is let go at once, below)
                // (held even when already locked on, so lowering always lets the lock go)
                holding = true;
                lockSeen = false;
                heldFrom = step;
                upRef = rel.y;
                haveUpRef = true;
                LOG("[vr] shield: raised at %.2f from the head; locking onto the nearest enemy (ZL held)%s", double(rel.y),
                    locked_on(lk) ? " (already locked on)" : "");
            }
            if (down != wasDown) {
                sinceChange = 0;
                heightCount = 0;  // (the window starts again from here)
            }
            if (holding) {
                if (locked_on(lk) && !lockSeen) {
                    lockSeen = true;
                    lockSeenAt = step;
                    upRef = rel.y;  // (the place it first locked on: 5 cm under it lets go)
                    LOG("[vr] shield: locked on with the hand at %.2f from the head; lowering it to %.2f lets go",
                        double(rel.y), double(rel.y - kShieldUnlockDrop));
                }
                const bool noTarget = !lockSeen && ld32(lk + kCurProc) == 7;  // (daPyProc_ATN_MOVE_e: Z, nothing locked)
                // (asked for after a play test, 2026-11-04: moved too far from the enemy, the lock let go, else his body's
                // facing broke) the locked target farther than kShieldLockMaxDistance, flat
                float targetDistance = 0;
                if (const uint32_t target = lockSeen ? ld32(lk + kLockOnActor) : 0) {
                    const Vec3 d = ld_vec(target + kPos) - ld_vec(lk + kPos);
                    if (finite(d)) targetDistance = std::sqrt(d.x * d.x + d.z * d.z);
                }
                const bool tooFar = targetDistance > kShieldLockMaxDistance;
                // (and only a target in front of the head, within kShieldLockFrontDegrees: the game chooses the target
                // and, with several people about, chose one at the side or behind, turned him round to it and the arms
                // took the animation; play test 2026-11-04, "make sure the player's body is not turning around")
                bool behind = false;
                // (judged as the lock comes, its first steps: looking round afterwards keeps it)
                if (const uint32_t target = lockSeen && step <= lockSeenAt + 2 ? ld32(lk + kLockOnActor) : 0) {
                    const Vec3 d = ld_vec(target + kPos) - ld_vec(lk + kPos);
                    if (finite(d) && d.x * d.x + d.z * d.z > 1.0f) {
                        const float headFacing = link_facing(g_map.heading, yaw_of(head.head.orientation));
                        const float off = std::fabs(wrap_radians(std::atan2(d.x, d.z) - headFacing));
                        behind = off > kShieldLockFrontDegrees * kPi / 180.0f;
                        if (behind)
                            LOG("[vr] shield: the target is %.0f degrees from where you look (over %.0f): let go",
                                double(off * 180.0f / kPi), double(kShieldLockFrontDegrees));
                    }
                }
                // (the game's target gone for kShieldLockLostSteps, never in a parry's move: ZL kept through a step or two
                // without it, as in the parry's roll and its cut, so the lock is not let go there; asked for, 2026-11-05:
                // "make sure the player doesn't lose the lock on of the enemy when he is pressing A to do the parry")
                // (long enough for the presses that take a target again, g_lockLostAt: 2 + 15 x 3 steps)
                constexpr uint64_t kShieldLockLostSteps = 50;
                static uint64_t lockLostAt = 0;
                if (!lockSeen || locked_on(lk) || parry_move_proc(ld32(lk + kCurProc))) lockLostAt = 0;
                else if (!lockLostAt) lockLostAt = step;
                const bool lockLost = lockLostAt && step >= lockLostAt + kShieldLockLostSteps;
                if (lockLost || noTarget || tooFar || behind ||
                    (!lockSeen && step > heldFrom + kShieldLockWait)) {
                    holding = false;
                    haveUpRef = false;  // (nothing held: the next lowering by the motion rules)
                    if (behind) {
                    } else if (tooFar)
                        LOG("[vr] shield: the target is %.0f units away (over %.0f): the lock-on let go", double(targetDistance),
                            double(kShieldLockMaxDistance));
                    else
                        LOG("[vr] shield: %s", lockSeen ? "the lock-on ended" : noTarget ? "nothing to lock onto (let go)"
                                                                             : "no lock-on came");
                }
                // (diagnostics: the target's distance once a second while held, for tuning the limit)
                static auto lastDistLog = std::chrono::steady_clock::time_point{};
                if (holding && lockSeen && std::chrono::steady_clock::now() - lastDistLog > std::chrono::seconds(1)) {
                    lastDistLog = std::chrono::steady_clock::now();
                    LOG("[vr] shield: locked on, the target %.0f units away", double(targetDistance));
                }
            }
            // (the lock let go while the stick is pushed back: the stick held in the middle until the next step, so the
            // game, no longer locked on, does not turn him round to walk away the step it lets go; the walk backwards,
            // g_backStep, takes him from there, still facing the way he faced. He turned round and back, 154 degrees at
            // once, a hitch in every distance release of a play test, 2026-11-04)
            static bool wasHolding = false;
            static uint64_t stillUntil = 0;
            if (wasHolding && !holding && (g_stickBack || g_backStep)) {
                stillUntil = step + 1;
                LOG("[vr] shield: let go while moving back: the stick held still for a step (no turn round)");
            }
            wasHolding = holding;
            if (step <= stillUntil) s.lx = s.ly = 0;
            if (holding) s.buttons |= pad::kZL;
            // The block (asked for, 2026-11-10: "raise shields then shield hand grip lowers us to guard"): with the shield
            // raised, the shield hand's grip held is the game's own block, ZR held (the GameCube's R): Link crouches
            // behind the shield (procCrouchDefense) and blocks from the front, as in the original game; let go, he stands
            // again. (The right stick held down is ZR too, as before, in either view)
            static bool guardGripWas = false;
            const bool guardGrip = !down && g_gripLatch[off].closed;
            if (guardGrip != guardGripWas)
                LOG("[vr] shield: %s", guardGrip ? "the grip held with the shield raised: the block (ZR held, crouched behind the shield)"
                                                 : "the grip let go: the block ends");
            guardGripWas = guardGrip;
            // Locked on, the raised shield guards by itself (asked for, 2026-11-10: "when the player raises his shield and is
            // locked on to an enemy then the player should also be in guard mode; if there is no enemy then the player needs
            // to hold the shield up and shield grip to guard"): the shield raised and its lock-on on an enemy holds ZR, as
            // the grip does. Not while he attacks: a swing's or parry's press (A or B this read), the parry's move, and
            // kGuardAfterAttack steps after either, so the cut is made standing and the guard comes back after it
            constexpr uint64_t kGuardAfterAttack = 15;  // (0.5 s)
            static uint64_t attackedAt = 0;
            const bool attacking = (s.buttons & (pad::kA | pad::kB)) != 0 || parry_move_proc(ld32(lk + kCurProc));
            if (attacking) attackedAt = step;
            const bool lockedGuard = !down && holding && lockSeen && locked_on(lk) && !attacking &&
                                     (attackedAt == 0 || step > attackedAt + kGuardAfterAttack);
            static bool lockedGuardWas = false;
            if (lockedGuard != lockedGuardWas && !guardGrip)
                LOG("[vr] shield: %s", lockedGuard ? "raised and locked on: the guard (ZR held)"
                                                   : attacking ? "an attack: the guard let go for it" : "the locked-on guard ends");
            lockedGuardWas = lockedGuard;
            if (guardGrip || lockedGuard) s.buttons |= pad::kZR;
        }
        if (g_lockLostAt && (s.buttons & pad::kZL) && first_person_play()) {
            if (g_relockTries < kRelockTries && step >= g_lockLostAt + kRelockFirst + kRelockEvery * g_relockTries) {
                s.buttons &= ~pad::kZL;  // (let go for this read; held again on the next: a new press)
                ++g_relockTries;
                LOG("[vr] lock-on: ZL pressed again to take a target (%d of %d)", g_relockTries, kRelockTries);
            }
        } else if (!(s.buttons & pad::kZL)) {
            g_lockLostAt = 0;  // (ZL let go by the player: nothing to take again)
        }
        if (g_lockLostAt && g_relockTries >= kRelockTries && step > g_lockLostAt + kRelockFirst + kRelockEvery * kRelockTries)
            g_lockLostAt = 0;
        g_padZL = (s.buttons & pad::kZL) != 0;  // (diagnostics: the lock-on's losses)
        g_padA = (s.buttons & pad::kA) != 0;    // (diagnostics: A in a parry's window)
        g_padZLShield = usable && holding;
    }
    // right stick: turning; while Link aims an item (bow, hookshot, boomerang) it aims as on the GamePad
    const uint32_t link = g_link.link;
    // The hookshot and the grappling hook in first person with their hand tracked: the hand aims them (subject_angle
    // overrides the stick's aim), so the stick turns the view and Link with it, as without them (reported in the
    // headset: holding either, he could not turn). Pointing follows the turn (g_map)
    const uint16_t aimItem = link ? ld16(link + 0x69B0) : 0;
    // (the bow too: it aims along its arrow, between the hands, bow_aim; asked for: turn with the right stick while the
    // bow is out)
    // (and the boomerang, as the hookshot, on land and on the boat: taken out, Link aims it at once (BOOMERANG_SUBJECT
    // 0x80, on the boat SHIP_BOOMERANG 0x8B, also while it flies), so the stick never turned while it was out; asked
    // for, 2026-11-06: "can't rotate ... holding the boomerang", "the hookshot ... can rotate". Its own aiming procedure
    // counts too, whatever is equipped: vr_sword.h aimed_item)
    const uint16_t handAimItem = aimed_item(link ? ld32(link + kCurProc) : 0, aimItem);
    const bool aimByHand = (handAimItem == kItemHookshot || handAimItem == kItemGrapplingHook || bow_in_hand(handAimItem) ||
                            handAimItem == kItemBoomerang) &&
                           !g_thirdPerson.load() &&
                           g_input.valid && g_input.hand[aim_side(c, handAimItem)].active;
    const bool aiming = link && mods::camera_mouse::aiming_item(ld32(link + kCurProc)) && !aimByHand;
    // on a ladder or a ledge in first-person play the view's base turns to Link (docs/VR_BODY_FOLLOW.md, "Ladders",
    // "Ledges")
    const bool ladder = link && g_link.valid && g_head.calibrated() && first_person_play() &&
                        rig_follows_link_proc(ld32(link + kCurProc));
    // locked onto a person, an item or an enemy in first-person play: the view's base turns to the target
    // (docs/VR_BODY_FOLLOW.md, "Lock-on"); the right stick waits, as on a ladder
    // the spin attack and the Hurricane Spin in first-person play: the view's base turns with Link (docs/VR_CONTROLS.md,
    // "Spin attacks")
    const bool spin = c.spinCamera && !ladder && link && g_link.valid && g_head.calibrated() && first_person_play() &&
                      spin_attack_proc(ld32(link + kCurProc));
    const bool lockOnPossible = !ladder && haveHead && link && g_link.valid && g_head.calibrated() && first_person_play();
    const bool lockOn = lockOnPossible && g_lockOn.update(ld32(link + kCurProc), yaw_of(head.head.orientation), locked_on(link));
    if (!lockOnPossible) g_lockOn.reset();
    const bool windCompass = aspect::wind_compass_shown();
    {
        static bool wasCompass = false;
        if (windCompass != wasCompass) {
            wasCompass = windCompass;
            LOG("[vr] wind compass: %s", windCompass ? "shown in the middle of the view; the right stick points it"
                                                     : "closed");
        }
    }
    {
        static bool wasLockOn = false;
        if (lockOn != wasLockOn) {
            wasLockOn = lockOn;
            LOG("[vr] lock-on: %s", lockOn ? "the view turns to bring the target in front of where you looked"
                                           : "ended; the view is free again");
        }
    }
    g_boatAimStick = !wheel && aiming && boat_view_mode(g_view.mode) && aspect::gameplay_camera();
    if (wheel) {
        // the item wheel chooses with it: no turning, nothing to the game
    } else if (g_boatAimStick) {
        // aiming on the boat (the bow, the boomerang, the hookshot: Link's ship procedures 0x8B to 0x8D turn him to the
        // game's camera, setBodyAngleToCamera) in the neck view or the Free Roam Camera: the right stick aims, as in third
        // person; the view's turn and slide wait. They had taken the stick, so the aim could not be moved (2026-11-06)
        s.rx = std::clamp(s.rx + right.stickX, -1.0f, 1.0f);
        s.ry = std::clamp(s.ry + right.stickY, -1.0f, 1.0f);
        g_turner.update(0, dt, c);
    } else if (g_view.mode == ViewMode::FreeRoam && aspect::gameplay_camera()) {
        // the Free Roam Camera: the right stick slides it (CameraOverride), nothing to the game's camera
        g_turner.update(0, dt, c);
    } else if (g_view.mode == ViewMode::Neck && aspect::gameplay_camera()) {
        // the neck view: the right stick turns the view as in first person (vr.turn: smooth each frame in CameraOverride,
        // or snap here), about the view's place on the neck; nothing to the game's camera
        if (c.turn == Turn::Smooth)
            g_smoothTurn.read = std::chrono::steady_clock::now();
        else
            g_head.boat_turn(g_turner.update(right.stickX, dt, c));
    } else if (g_baton.conducting()) {
        float x = 0, y = 0;
        baton_stick(baton.dir[1], x, y);  // the beat: the game's C-stick, left 3/4, right 6/4
        s.rx = std::clamp(s.rx + x, -1.0f, 1.0f);
        s.ry = std::clamp(s.ry + y, -1.0f, 1.0f);
        g_turner.update(0, dt, c);  // no turning while conducting: the stick waits
    } else if (windCompass) {
        // the wind compass after the Wind's Requiem: the right stick points it, as the left stick does (the game reads
        // the left stick's direction); no turning meanwhile
        const Stick point = dead_zone({right.stickX, right.stickY}, c.stickDeadZone);
        if (point.x != 0 || point.y != 0) {
            s.lx = std::clamp(point.x, -1.0f, 1.0f);
            s.ly = std::clamp(point.y, -1.0f, 1.0f);
        }
        g_turner.update(0, dt, c);
    } else if (aiming) {
        s.rx = std::clamp(s.rx + right.stickX, -1.0f, 1.0f);
        s.ry = std::clamp(s.ry + right.stickY, -1.0f, 1.0f);
    } else if (g_thirdPerson.load() && aspect::gameplay_camera()) {
        s.rx = std::clamp(s.rx + right.stickX, -1.0f, 1.0f);  // third person: turns the game's camera, as on the GamePad
    } else if (ladder || lockOn) {
        g_turner.update(0, dt, c);  // on a ladder or ledge, or locked on, the view's base belongs to Link (below): the stick waits
    } else if (c.turn == Turn::Smooth) {
        g_smoothTurn.read = std::chrono::steady_clock::now();  // (turned by each frame: CameraOverride)
    } else {
        const float turn = g_turner.update(right.stickX, dt, c);
        g_head.turn(turn);  // (a snap: frame_correct turns Link's body with it every pass)
    }
    g_smoothTurn.allowed = c.turn == Turn::Smooth && !wheel && !g_baton.conducting() && !windCompass && !aiming &&
                           !(g_thirdPerson.load() && aspect::gameplay_camera() && g_view.mode != ViewMode::Neck) &&
                           !ladder && !lockOn && !spin;
    // the body's facing (docs/VR_BODY_FOLLOW.md): in first-person play it is Link's while the game turns him (moving,
    // lock-on, attacks, items, events: his heading back into the tracking space), and follows the head while he
    // stands (link_executing then turns him with it); elsewhere (third person, menus) it follows the head for the
    // shoulders and swings as before
    g_head.stop_rig_turn();  // (set again below on a ladder or ledge or during a lock-on)
    if (haveHead) {
        const uint32_t l = g_link.link;
        const bool gameTurns = l && g_link.valid && g_head.calibrated() && first_person_play() &&
                               (!body_follow_proc(ld32(l + kCurProc)) || g_stickBack);  // (backwards: body_follows_head)
        const bool was = g_body.following;
        if (g_baton.conducting() && conducting_first_person(l)) {
            // conducting in first person: the body (and his head with it) faces where the hands are held, forward,
            // with no more slack than 5 degrees (asked for in the headset). The hands' places when conducting began,
            // not where they are now: the beat moves the right hand left and right, and the body would sway with it.
            // Checked before gameTurns: where the game counts conducting as normal play, that took his facing from
            // the game, which turns him round when he conducts (in VR: backwards; seen in the headset)
            float yaw = yaw_of(head.head.orientation);
            Vec3 at;
            const bool settled = g_baton.hands_centre(at);
            if (!settled && (left.active || right.active))
                at = left.active && right.active ? (left.grip.position + right.grip.position) * 0.5f
                     : left.active                ? left.grip.position
                                                  : right.grip.position;
            const Vec3 d = at - head.head.position;
            if ((settled || left.active || right.active) && std::sqrt(d.x * d.x + d.z * d.z) > 0.1f)
                yaw = std::atan2(-d.x, -d.z);  // (as yaw_of: forward -z)
            g_body.update(yaw, dt, 0);
            // (diagnostics) once a second while conducting: where the body is turned to, against Link's facing
            static auto lastLog = std::chrono::steady_clock::time_point{};
            if (const auto now = std::chrono::steady_clock::now(); now - lastLog > std::chrono::seconds(1)) {
                lastLog = now;
                const float deg = 180.0f / kPi;
                LOG("[vr] Wind Waker body: hands %.0f (%s), head %.0f, body %.0f -> Link should face %.0f; he faces %.0f "
                    "(model) %.0f (heading)",
                    double(yaw * deg), settled ? "settled" : "now", double(yaw_of(head.head.orientation) * deg),
                    double(g_body.yaw * deg), double(link_facing(g_map.heading, g_body.yaw) * deg),
                    double(s16_to_radians(int16_t(ld16(l + kShapeAngleY))) * deg),
                    double(s16_to_radians(int16_t(ld16(l + kHeading))) * deg));
            }
        } else if (spin) {
            // a spin attack or the Hurricane Spin: the view's base follows his model's facing round (turned each frame,
            // CameraOverride, fast enough to keep up with a turn in half a second), the body keeping the yaw it had when
            // the spin began, as on a ladder, so the head still looks round freely; when the spin ends he faces where he
            // began and the view is free again
            g_body.following = false;
            // (his model as drawn: the spin attack turns him in its animation and leaves his facing, shape_angle, as it
            // was; the Hurricane Spin adds its turn to the facing only at its end (procCutTurn, procCutRoll). Followed
            // by the line between his shoulders, the clavicles' origins, which the arm tracking never moves: its turn
            // since the spin began, added to the facing he began with. The facing itself if the joints are not there;
            // the view did not turn at all with it alone: reported in the headset, 2026-11-04)
            static bool spinWas = false;
            static float spinFacing0 = 0, spinShoulders0 = 0;
            static float spinDir = 0;  // the spin attack's way round: the game's sword's sweep (+1 / -1; 0 not yet)
            const float shapeFacing = s16_to_radians(int16_t(ld16(l + kShapeAngleY)));
            // (taken where his body is drawn, before Hide Link's body / arms fold the shoulders onto his neck: read
            // from his matrices here, both shoulders were one point and the view stayed still; play test, 2026-11-04)
            // (the sword first: the game's sword tip as his animation swings it round; the shoulders as drawn if not)
            const float swordYaw = g_gameSwordYaw.load(std::memory_order_relaxed);
            const float drawnShoulders = g_drawnShoulders.load(std::memory_order_relaxed);
            const bool bySword = std::isfinite(swordYaw);
            const float shoulders = bySword ? swordYaw : drawnShoulders;
            const bool haveShoulders = std::isfinite(shoulders);
            // (diagnostics: every few steps of a spin, both sources and his model's joints from the game's own matrices)
            {
                static int tick = 0;
                if (!g_spinActive) tick = 0;
                if (tick++ % 5 == 0) {
                    uint16_t n = 0;
                    const uint32_t body = matrices_of(ld32(l + kBodyModel), n);
                    Vec3 across{};
                    float y0 = NAN, y1 = NAN;
                    if (body && n > kClavicleJoints[1]) {
                        across = read_joint(body, kClavicleJoints[1]).pos - read_joint(body, kClavicleJoints[0]).pos;
                        y0 = yaw_of(read_joint(body, 0).rot);
                        y1 = yaw_of(read_joint(body, 1).rot);
                    }
                    LOG("[vr] spin diag: step %llu action 0x%X facing %.0f; sword %.0f; drawn shoulders %.0f; game matrices %u joints, "
                        "shoulders %.1f apart yaw %.0f, joint 0 yaw %.0f, joint 1 yaw %.0f", (unsigned long long)interp::logic_steps(),
                        ld32(l + kCurProc), double(shapeFacing * 180.0f / kPi), double(swordYaw * 180.0f / kPi),
                        double(drawnShoulders * 180.0f / kPi), unsigned(n),
                        double(length(across)), double(std::atan2(across.x, across.z) * 180.0f / kPi),
                        double(y0 * 180.0f / kPi), double(y1 * 180.0f / kPi));
                }
            }
            static float spinLast = 0;
            if (!g_spinActive) {
                spinDir = 0;
                g_spinActive = true;
                spinFacing0 = shapeFacing;
                spinShoulders0 = shoulders;
                spinWas = haveShoulders;
                spinLast = shoulders;
                g_spinTurned = 0;
                g_spinFromShoulders = haveShoulders;
                g_spinBySword = bySword;
            } else if (haveShoulders && spinWas) {
                g_spinTurned += wrap_radians(shoulders - spinLast);  // (diagnostics: the log at its end)
                spinLast = shoulders;
            }
            // The turn itself, as the game makes it (asked for: "when the sword animation starts, do a 360 turn at the
            // same speed as the sword's"; procCutTurn, procCutRoll):
            // - the spin attack (0x55): one whole turn over the animation's cutting frames, 3 to 18.5 of
            //   mFrameCtrlUnder[0] (the attack's cylinder armed, the kaiten-giri sound, the trail fading at 18.5), by its
            //   frame, so at the sword's own speed; the way the game's sword swept (else the Hurricane Spin's way, m34EC
            //   counting down)
            // - the Hurricane Spin (0x56): his facing plus m34EC, the turn the game adds to his model each step and into
            //   his facing at its end; its end (0x57): the facing
            constexpr uint32_t kFrameCtrlUnderFrame = 0x5898 + 4, kSpinTurn = 0x6934;  // (mFrameCtrlUnder[0].mFrame, m34EC)
            constexpr float kCutFirst = 3.0f, kCutLast = 18.5f;
            const uint32_t spinProc = ld32(l + kCurProc);
            float facing = shapeFacing;
            if (spinProc == 0x55) {
                if (spinDir == 0 && haveShoulders && spinWas && std::fabs(g_spinTurned) > 0.09f)
                    spinDir = g_spinTurned > 0 ? 1.0f : -1.0f;  // (the sword's sweep, once it has gone 5 degrees)
                const float frame = float(ldf32(l + kFrameCtrlUnderFrame));
                const float share = std::isfinite(frame) ? std::clamp((frame - kCutFirst) / (kCutLast - kCutFirst), 0.0f, 1.0f)
                                                         : 0.0f;
                facing = wrap_radians(spinFacing0 + (spinDir != 0 ? spinDir : -1.0f) * 2.0f * kPi * share);
            } else if (spinProc == 0x56) {
                facing = wrap_radians(shapeFacing + s16_to_radians(int16_t(ld16(l + kSpinTurn))));
            }
            g_head.set_rig_turn({true, false, rig_heading_for(facing, g_body.yaw), kSpinTurnResponse});
        } else if (ladder) {
            // on a ladder or a ledge the view's base turns until the body's facing is Link's (his model's: climbing
            // down or stepping off an edge the game turns him round to the ladder or wall; sliding along a ledge he
            // keeps facing the wall while he moves sideways); the body keeps the yaw it had when he got on, so the head
            // looks round freely and cannot undo it
            g_body.following = false;
            // (sidling along a wall: his animated head's yaw, which looks along the wall while his model faces out)
            const float facing = rig_facing(ld32(l + kCurProc), s16_to_radians(int16_t(ld16(l + kShapeAngleY))),
                                            g_link.headToEye);
            g_head.set_rig_turn({true, false, rig_heading_for(facing, g_body.yaw)});  // (turned each frame: CameraOverride)
        } else if (lockOn) {
            // locked on: the game turns Link to the target (his model's facing). The view's base turns, the short way
            // and eased (as on a ladder), until that facing maps onto the anchor, the head's yaw when the lock began;
            // the body's yaw stays Link's heading, as when the game turns him, so it reaches the anchor with the base.
            // The head's own turns change neither: it looks round freely, and the headset's pose is never touched
            float facing = s16_to_radians(int16_t(ld16(l + kShapeAngleY)));
            // in a parry's move (docs/VR_CONTROLS.md, "Parry attacks") towards the enemy itself, not his facing: he jumps
            // over it facing his take-off's way and lands behind it, where the view is to face its back to finish the
            // attack (asked for in the headset); turned faster, keeping up with the move
            const bool parry = parry_move(ld32(l + kCurProc));
            if (parry) {
                const uint32_t target = ld32(l + kLockOnActor);
                const Vec3 to = target ? ld_vec(target + kPos) - ld_vec(l + kPos) : Vec3{};
                if (target && finite(to) && (std::fabs(to.x) + std::fabs(to.z)) > 1.0f) facing = std::atan2(to.x, to.z);
            }
            g_head.set_rig_turn({true, true, rig_heading_for(facing, g_lockOn.anchor), parry ? 10.0f : 4.0f});  // (turned each frame: CameraOverride)
            g_body.set(body_yaw_of(g_link.heading, g_map.heading));
        } else if (gameTurns) {
            g_body.set(body_yaw_of(g_link.heading, g_map.heading));
        } else {
            // with Detached Arms the body also turns towards where the hands reach (the arms may leave the shoulders,
            // the body still follows them)
            float follow = yaw_of(head.head.orientation);
            if (c.detachedArms) {
                const bool both = left.active && right.active;
                const Vec3 mid = both ? (left.grip.position + right.grip.position) * 0.5f
                                      : left.active ? left.grip.position : right.grip.position;
                follow = body_follow_yaw(follow, left.active || right.active, mid - head.head.position);
            }
            // standing in first-person play it faces the head at once: no slack, no easing. The turn is about his eyes
            // (room_scale_move), so it no longer swings the view, which the slack was for. In every first-person mode,
            // seated or standing, with vr.roomScale on or off (that only moves him when you walk)
            // (for kAfterLockTurnSeconds after a lock-on ends he turns to the head smoothly, not at once: the head had
            // looked away from the target while he faced it, and the lock let go: he turned 155 degrees in one step, a
            // hitch at every release in a play test, 2026-11-04)
            constexpr float kAfterLockTurnSeconds = 1.0f;
            static float sinceLock = 1e9f;
            sinceLock = lockOn ? 0.0f : sinceLock + dt;
            if (l && g_link.valid && g_head.calibrated() && first_person_play() && body_follow_proc(ld32(l + kCurProc))) {
                if (sinceLock < kAfterLockTurnSeconds) g_body.set(turn_towards(g_body.yaw, follow, dt, 8.0f, 3.0f));
                else g_body.set(follow);
            }
            else
                g_body.update(follow, dt, c.bodyFollowDegrees * kPi / 180.0f);
        }
        static bool wasSpin = false;
        if (!spin) g_spinActive = false;
        if (spin != wasSpin) {
            wasSpin = spin;
            if (spin)
                LOG("[vr] spin attack: the view turns round with Link (action 0x%X)", l ? ld32(l + kCurProc) : 0u);
            else
                LOG("[vr] spin attack: over, the view is free again; it followed %s, %.0f degrees",
                    g_spinBySword ? "the game's sword" : g_spinFromShoulders ? "his shoulders" : "his facing (no sword, no shoulders)",
                    double(g_spinTurned * 180.0f / kPi));
        }
        static bool wasLadder = false;
        if (ladder != wasLadder) {
            wasLadder = ladder;
            if (ladder)
                LOG("[vr] %s: the view turns to face the way Link faces", sidle_proc(ld32(l + kCurProc)) ? "wall (his head)"
                    : hang_proc(ld32(l + kCurProc)) ? "ledge" : "ladder");
            else
                LOG("[vr] ladder/ledge: the view is free again");
        }
        // WWHD_VR_TRACE_LEDGE=1: twice a second on a ladder or ledge, Link's action, position, facing and travel and
        // the view's base, the head's own yaw and the yaw they make together (all degrees)
        static const bool traceLedge = [] { const char* e = std::getenv("WWHD_VR_TRACE_LEDGE"); return e && *e == '1'; }();
        static int ledgeTick = 0;
        if (traceLedge && ladder && ++ledgeTick % 45 == 1) {
            const float deg = 180.0f / kPi, headYaw = yaw_of(head.head.orientation);
            LOG("[vr] ledge: proc 0x%X at %.0f %.0f %.0f facing %.0f travel %.0f frame %.0f; base %.0f body %.0f head %.0f "
                "view %.0f",
                ld32(l + kCurProc), double(g_link.pos.x), double(g_link.pos.y), double(g_link.pos.z),
                double(s16_to_radians(int16_t(ld16(l + kShapeAngleY))) * deg),
                double(s16_to_radians(int16_t(ld16(l + kHeading))) * deg), double(g_link.heading * deg),
                double(g_map.heading * deg), double(g_body.yaw * deg), double(headYaw * deg),
                double(wrap_radians(g_map.heading + headYaw) * deg));
        }
        static const bool trace = [] { const char* e = std::getenv("WWHD_VR_TRACE_BODY"); return e && *e == '1'; }();
        if (trace && g_body.following != was)
            LOG("[vr] body: %s (head %.0f degrees from it)", g_body.following ? "follows the head" : "faces the head",
                double(wrap_radians(yaw_of(head.head.orientation) - g_body.yaw) * 180.0f / kPi));
    }
    // the TV's layouts outside normal play: the menu scenes' (title, file select) and the text and textboxes of
    // conversations and cutscenes drawn smaller (as if farther), the text optionally moved up or down; the gameplay
    // HUD keeps its size
    const bool gameplay = aspect::gameplay_or_conducting();  // (a conversation in first person: its text, shrunk)
    const bool talk = !gameplay && talk_first_person();       // (its text held in the world: CameraOverride)
    const bool menu = !gameplay && menu_stage(stage_name());
    // the telescope: its eyepiece (the game's mask: black around a round view, the zoom gauge) at full size and
    // enlarged until its black covers the whole eye picture, which is taller than the 16:9 layout space
    const bool scope = telescope_view();
    // a Picto Box (docs/VR_PICTO_BOX.md): its viewfinder (the black frame around the picture's window, the zoom gauge,
    // the count) at vr.pictoBoxSize of its full size, centred (picto_frame_scale; full size put the zoom gauge outside
    // what the lenses show). The photo is the part inside the frame (aspect.cpp), so it shrinks with it. No eyepiece fill
    const bool picto = !scope && picto_view();
    float layoutScale = picto ? picto_frame_scale() : gameplay ? 1.0f : 1.0f / (menu ? c.menuZoomOut : c.textZoomOut);
    if (scope) {
        // the eyepiece at vr.telescopeSize of the size that covers the eye; outside the mask's own black, the eye
        // copies fill black (vr_vulkan.cpp), so its ring stays inside what the lenses show
        Fov f;
        const float eyeAspect = eye_image_aspect() > 0 ? std::max(0.5f, eye_image_aspect())  // (the eye pictures' shape)
                                : projection(f) ? std::max(0.5f, fov_aspect(f)) : 16.0f / 9.0f;
        const float cover = std::max(1.0f, (16.0f / 9.0f) / eyeAspect);  // the layout's height share is 1/cover
        layoutScale = cover * c.telescopeSize;
        set_eyepiece(std::min(1.0f, layoutScale * (eyeAspect >= 16.0f / 9.0f ? (16.0f / 9.0f) / eyeAspect : 1.0f)),
                     std::min(1.0f, layoutScale / cover));
    } else if (picto) {
        // the viewfinder at its smaller size: outside the layout's area the eye copies fill black, as around the
        // telescope's eyepiece, so the frame's black reaches the edges of the view (the 3D window around the smaller
        // frame no longer shows; seen in the headset)
        // (the shares are of the UI layer's image, where the viewfinder is drawn: the drawn field's shape, which the
        // eye copies place per eye, vr_vulkan.cpp eye_part)
        Fov f;
        const float uiAspect = projection(f) ? std::max(0.5f, fov_aspect(f))
                               : eye_image_aspect() > 0 ? std::max(0.5f, eye_image_aspect()) : 16.0f / 9.0f;
        const float cover = std::max(1.0f, (16.0f / 9.0f) / uiAspect);  // the layout's height share is 1/cover
        set_eyepiece(std::min(1.0f, layoutScale * (uiAspect >= 16.0f / 9.0f ? (16.0f / 9.0f) / uiAspect : 1.0f)),
                     std::min(1.0f, layoutScale / cover));
    } else {
        set_eyepiece(0, 0);
    }
    set_layout_scale(layoutScale);
    if (!talk) {  // (a conversation in first person: both offsets set each frame by CameraOverride, talk_text_anchor)
        set_layout_offset_y(gameplay || menu || scope || picto ? 0.0f : c.textHeight);
        set_layout_offset_x(0.0f);
    }
    static int lastKind = -1;
    static uint64_t g_textSince = 0;
    static bool g_textShot = false;
    const int kind = scope ? 3 : picto ? 4 : gameplay ? 0 : menu ? 1 : 2;
    if (kind != lastKind) {
        lastKind = kind;
        LOG("[vr] layouts: %s, drawn at %.2f of their size (frame %llu)",
            kind == 3 ? "telescope eyepiece" : kind == 4 ? "Picto Box viewfinder" : kind == 0 ? "gameplay HUD" : kind == 1 ? "menu scene" : "conversation / cutscene text", double(layoutScale),
            static_cast<unsigned long long>(interp::logic_steps()));
        g_textSince = interp::logic_steps();
        g_textShot = false;
    }
    // test: WWHD_VR_SHOT_TEXT=1 takes a screenshot 2 s into each conversation or cutscene (its text typed out)
    static const bool shotText = [] { const char* e = std::getenv("WWHD_VR_SHOT_TEXT"); return e && *e == '1'; }();
    if (shotText && kind == 2 && !g_textShot && g_textSince > 600 && interp::logic_steps() >= g_textSince + 60) {
        g_textShot = true;
        screenshot::request();
    }
}

// ---- movement angle
int16_t controlled_angle_hook(int16_t original) {
    if (!running()) return original;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_head.calibrated() || !g_haveWanted || !g_haveCameraHeading || !aspect::gameplay_camera()) return original;
    if (g_thirdPerson.load()) return original;  // third person: the stick moves relative to the game's camera
    if (!g_stickPushed) {
        // the stick is released: the game's angle follows its camera; learn the constant between the two
        const int16_t measured = int16_t(uint16_t(original) - uint16_t(radians_to_s16(g_cameraHeading)));
        const int16_t snapped = snap_angle_offset(measured);
        if (snapped == measured || (uint16_t(snapped) & 0x3FFF) == 0) {
            if (!g_haveAngleOffset || snapped != g_angleOffset)
                LOG("[vr] movement: the game's angle is the camera's heading + 0x%04X", unsigned(uint16_t(snapped)));
            g_angleOffset = snapped;
            g_haveAngleOffset = true;
        }
    }
    if (!g_haveAngleOffset) return original;
    return controlled_angle(g_wantedHeading, g_angleOffset);
}

// ---- arms
bool shield_hand(const Joint& to, const Config& c, int d, Quat& handRot);  // (below: the shield hand on its handle)
void link_arms(uint32_t link) {
    GameWorkTimer workTimer(GameWork::Arms);  // (docs/VR_PERFORMANCE.md: the VR work on the game thread)
    if (!running() || !link) return;
    const Config c = config();
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint32_t model = ld32(link + kBodyModel);
    const uint32_t block = model ? ld32(model + kMtxBlock) : 0;
    const uint32_t mtx = block ? ld32(block + kMtxBlockMatrices) : 0;
    if (!mtx || ld16(block + 0x2C) <= 12) return;
    auto read = [&](int j) {
        float m[12];
        for (int i = 0; i < 12; ++i) m[i] = float(ldf32(mtx + j * 48 + i * 4));
        return joint_from_matrix(m);
    };
    auto write = [&](int j, const Joint& joint) {
        float m[12];
        matrix_from_joint(joint, m);
        for (int i = 0; i < 12; ++i) stf32(mtx + j * 48 + i * 4, m[i]);
    };
    if (g_armsCheckedLink != link) {
        // the joint numbers must give two arms of believable bone lengths (game units; Link's arm is about 30)
        g_armsCheckedLink = link;
        g_armsOk = true;
        g_hingeLocal[0] = g_hingeLocal[1] = {};
        for (const auto& a : kArmJoints) {
            const ArmJoints arm{read(a[0]), read(a[1]), read(a[2])};
            const float u = length(arm.lower.pos - arm.upper.pos), l = length(arm.hand.pos - arm.lower.pos);
            LOG("[vr] arm joints %d/%d/%d: upper arm %.1f, forearm %.1f units", a[0], a[1], a[2], double(u), double(l));
            g_armsOk = g_armsOk && plausible_arm(arm, 3.0f, 80.0f);
        }
        if (!g_armsOk) LOG("[vr] Link's arm joints do not look like arms: the arms keep the animation");
        // which set is his left arm: its shoulder lies to the left of his heading (else the controllers would drive
        // the opposite arms and the elbows bend to the inside)
        if (g_armsOk) {
            // this Link's own position and heading (the step's record may still be another scene's)
            const Vec3 right = heading_right(s16_to_radians(int16_t(ld16(link + kHeading))));
            const Vec3 at = ld_vec(link + kPos);
            const float a = dot(read(kArmJoints[0][0]).pos - at, right);
            const float b = dot(read(kArmJoints[1][0]).pos - at, right);
            // only a clear answer (shoulders a believable distance apart) changes the assignment
            if (std::isfinite(a) && std::isfinite(b) && std::fabs(a - b) > 5.0f && std::fabs(a - b) < 60.0f) {
                const bool swapped = a > b;
                g_jointSet[0] = swapped ? 1 : 0;
                g_jointSet[1] = swapped ? 0 : 1;
            }
            LOG("[vr] arm sides: joints %d %.1f, joints %d %.1f units to Link's right: left arm %d", kArmJoints[0][0],
                double(a), kArmJoints[1][0], double(b), kArmJoints[g_jointSet[0]][0]);
            set_link_arm_length(0.5f * (length(read(kArmJoints[0][1]).pos - read(kArmJoints[0][0]).pos) +
                                        length(read(kArmJoints[0][2]).pos - read(kArmJoints[0][1]).pos) +
                                        length(read(kArmJoints[1][1]).pos - read(kArmJoints[1][0]).pos) +
                                        length(read(kArmJoints[1][2]).pos - read(kArmJoints[1][1]).pos)));
            LOG("[vr] world scale %.1f units per metre%s", double(world_scale()),
                vr::config().matchLinkBody ? " (matched to Link's arms)" : "");
            // each hand's straight-wrist direction: the forearm's, in the hand joint's frame (his idle pose)
            for (int set = 0; set < 2; ++set) {
                const Joint hand = read(kArmJoints[set][2]), fore = read(kArmJoints[set][1]);
                g_wristStraight[set] = normalize(rotate(conjugate(hand.rot), normalize(hand.pos - fore.pos)), {0, 0, 1});
                const Vec3 fwdIdle = heading_dir(s16_to_radians(int16_t(ld16(link + kHeading))));
                g_handLevelUp[set] = level_hand_up(hand.rot, g_wristStraight[set], fwdIdle);
                g_ctrlToHand[set] = controller_to_hand(level_hand(hand.rot, g_wristStraight[set], fwdIdle), fwdIdle);
                g_ctrlToHandOk[set] = std::isfinite(g_ctrlToHand[set].x) && std::isfinite(g_ctrlToHand[set].w);
            }
        }
        // the clavicles: a short bone from them to each upper arm (else the shoulders stay as animated)
        g_claviclesOk = g_armsOk;
        for (int side = 0; side < 2; ++side) {
            const float d = length(read(kArmJoints[side][0]).pos - read(kClavicleJoints[side]).pos);
            LOG("[vr] clavicle joint %d: %.1f units to the upper arm", kClavicleJoints[side], double(d));
            g_claviclesOk = g_claviclesOk && std::isfinite(d) && d > 1.0f && d < 25.0f;
        }
        if (g_armsOk && !g_claviclesOk) LOG("[vr] the clavicles do not check out: the shoulders keep the animation");
        // the hands' frames mirrored: Link's left hand's axes reflected across his body against his right hand's
        if (g_armsOk) {
            const Vec3 right = heading_right(s16_to_radians(int16_t(ld16(link + kHeading))));
            const Joint l = read(kArmJoints[0][2]), r = read(kArmJoints[1][2]);
            float dots[3];
            for (int a = 0; a < 3; ++a) {
                const Vec3 e{a == 0 ? 1.0f : 0.0f, a == 1 ? 1.0f : 0.0f, a == 2 ? 1.0f : 0.0f};
                Vec3 la = rotate(l.rot, e);
                la = la - right * (2.0f * dot(la, right));
                dots[a] = dot(la, rotate(r.rot, e));
            }
            const bool clear = std::fabs(dots[0]) > 0.5f && std::fabs(dots[1]) > 0.5f && std::fabs(dots[2]) > 0.5f;
            if (clear) {
                g_mirrorSign = {dots[0] > 0 ? 1.0f : -1.0f, dots[1] > 0 ? 1.0f : -1.0f, dots[2] > 0 ? 1.0f : -1.0f};
                g_mirrorKnown = true;
            }
            LOG("[vr] hands mirrored: axes %.2f %.2f %.2f%s", double(dots[0]), double(dots[1]), double(dots[2]),
                clear ? "" : " (not clear: the right hand's grip is not mirrored)");
        }
    }
    // Link's position and heading for this step. This hook runs inside his execute, before link_executed records
    // them in g_link: g_link.pos is still the step before. Measuring his head, shoulders and the arms' targets
    // against it put everything one step's travel behind him while he moved (measured with WWHD_VR_TRACE_LAG: the
    // hands' targets 17 to 23 cm behind the controllers while running, under 1 cm standing).
    const Vec3 stepPos = ld_vec(link + kPos);
    const float stepHeading = link_frame_heading(link);
    // third person: no motion controls, the arms play the game's animation (they fade over 0.15 s, as on a lost
    // controller, and back in first person)
    // Link's body held steady against the eyes in first person. The camera's eye height and forward offset are held
    // through the stride (the camera does not move on its own), but the running animation lifts and drops his whole
    // skeleton with every stride: under a still camera his body bobbed and shook while he moved (reported in the
    // headset). After his animation (this hook runs right after his model's calc) and before the arms and what the
    // game attaches to his hands, the skeleton is moved as a whole so his head joint stays at a settled place in his
    // own frame (his position and heading): through the stride's bob (measured 8 to 17 units a second; a 9-unit dead zone),
    // following real changes of posture over 0.3 s. Only where his bones are drawn changes, never his position,
    // collision or animation; third person and events show the game's own.
    if (running() && play_view() && !g_thirdPerson.load() && !c.showBody) {
        const uint16_t n = ld16(block + 0x2C);
        const Vec3 head = read(kHeadJointIndex).pos;
        const Vec3 fwd = heading_dir(stepHeading), right = heading_right(stepHeading);
        const Vec3 rel = head - stepPos;
        const Vec3 local{dot(rel, right), rel.y, dot(rel, fwd)};
        if (finite(local) && n > kHeadJointIndex && n <= 64) {
            if (!g_bodySeeded || g_bodyLink != link) {
                g_bodyHold.reset(local);
                g_bodySeeded = true;
                g_bodyLink = link;
            }
            g_bodyHold.update(local, 9.0f, kStepSeconds, 0.3f);
            const Vec3 held = g_bodyHold.value;
            Vec3 d = right * (held.x - local.x) + Vec3{0, held.y - local.y, 0} + fwd * (held.z - local.z);
            // at most 12 units, scaled rather than skipped (skipping a larger one, while the hold catches up with a
            // lean into a run, made the body jump between moved and not)
            if (length(d) > 12.0f) d = d * (12.0f / length(d));
            static const bool traceBody = [] { const char* e = std::getenv("WWHD_VR_TRACE_HAND"); return e && *e == '1'; }();
            if (traceBody) {
                static float lo = 1e9f, hi = -1e9f, worst = 0;
                static int m = 0;
                lo = std::min(lo, local.y);
                hi = std::max(hi, local.y);
                worst = std::max(worst, length(d));
                if (++m == 30) {
                    LOG("[vr] body trace: the animation's head rose and fell %.2f units in a second; moved back by up to %.2f",
                        double(hi - lo), double(worst));
                    m = 0; lo = 1e9f; hi = -1e9f; worst = 0;
                }
            }
            if (finite(d) && length(d) > 1e-4f) {
                for (uint32_t i = 0; i < n; ++i) {
                    const uint32_t at = mtx + i * 48;
                    stf32(at + 12, float(ldf32(at + 12)) + d.x);
                    stf32(at + 28, float(ldf32(at + 28)) + d.y);
                    stf32(at + 44, float(ldf32(at + 44)) + d.z);
                }
                st16(block + kMtxBlockFlags, uint16_t(ld16(block + kMtxBlockFlags) | kMatricesChanged));
            }
        }
    }
    const bool on = g_armsOk && c.armIK && g_head.calibrated() && g_input.valid && play_view() && !g_thirdPerson.load();
    xr::Views v;
    bool haveHead = on && game_views(display_time(), v);
    // the headset's pose through a short tracking gap too: the arms keep their targets (as the controllers do)
    {
        static xr::Views lastViews;
        static std::chrono::steady_clock::time_point lastAt{};
        const auto now = std::chrono::steady_clock::now();
        if (haveHead) {
            lastViews = v;
            lastAt = now;
        } else if (on && lastViews.valid && now - lastAt < std::chrono::seconds(2)) {
            v = lastViews;
            haveHead = true;
        }
    }
    bool changed = false;
    // detached arms (vr.detachedArms): eased in and out over a quarter of a second, so switching it never snaps
    g_detachWeight = std::clamp(g_detachWeight + (c.detachedArms ? 1.0f : -1.0f) * kStepSeconds / 0.25f, 0.0f, 1.0f);
    // the game's shield hand as its animation posed it, before the arms below: what the shield's own axes are learnt
    // from (measure_held)
    g_animShieldHand = read(kArmJoints[1][2]);
    g_animShieldHandFresh = true;
    g_animHand[0] = read(kArmJoints[0][2]);
    g_animHand[1] = g_animShieldHand;
    g_animHandFresh = true;
    // (Link turned away from the head: walking backwards, g_stickBack, the game turns him round to walk that way while
    // the controllers stay in front of the head, and each arm reached across his back for the controller on the other
    // side: the hands crossed, reported in the headset. While he faces more than 100 degrees from the head (back under
    // 80) the arms take the game's animation; his hands are behind the view then)
    // Not with the Deku Leaf (its fan swing and glide): the swing turns him round past 100 degrees, and at every use the
    // arms went to the animation and back, the hands and the shield on them popping into view (reported in the headset)
    // Nor while aiming an item (the bow, hookshot, boomerang, grappling hook: aiming_item, standing or moving): the game
    // turns him to the aim, and the bow's aim is the line between his hands (bow_aim). Turned away (that line pointing
    // back, the drawing hand past the bow hand), the arms took the animation, the aim came from the animation's hands
    // and kept him turned: the arms, the aim and the shots no longer followed the controllers until the bow was put
    // away (reported with a video, 2026-11-04: "frozen and I can't aim")
    static bool facesAway = false;
    const uint32_t armProc = ld32(link + kCurProc);
    // (nor in a spin attack or the Hurricane Spin: the view turns round with him, spin_attack_proc, and the arms, on the
    // controllers, with it; the spin turned his model past 100 degrees and both arms went to the animation and back in
    // every spin of a play test, 2026-11-04, the hand and sword not turning round with the view)
    // (nor locked on: the game turns him to the target while the view turns there too, eased; moving back locked on, the
    // turn left him past 100 degrees from the head and the arms took the animation, the sword and shield out of his
    // hands, play test 2026-11-04)
    // (and for kAfterLockSteps after a lock-on ends: moving back, the game let go of its target and turned him round
    // to walk away before the walk backwards, g_backStep, turned him back; the arms took the animation between: play
    // test, 2026-11-04)
    constexpr uint64_t kAfterLockSteps = 30;  // (a second: the smooth turn back to the head after it, too)
    static uint64_t lastLockedStep = 0;
    if (locked_on(link)) lastLockedStep = interp::logic_steps();
    const bool justLocked = lastLockedStep && interp::logic_steps() <= lastLockedStep + kAfterLockSteps;
    // (nor on the boat: he sits facing its way, so looking round from the tiller turned the arms to the animation and the
    // hands and what they held out of view; 2026-11-06, the boat's weapons to work as on land)
    if (armProc == kProcLeafSwing || armProc == kProcLeafGlide || mods::camera_mouse::aiming_item(armProc) ||
        ship_ride_proc(armProc) || spin_attack_proc(armProc) || locked_on(link) || justLocked ||
        // (nor while the stick is pushed back: the walk backwards, g_backStep, turns him to face the head, and the step or
        // two the game turns him round first took the arms to the animation, the sword and shield out of his hands)
        g_stickBack || g_backStep)
        facesAway = false;
    else if (haveHead) {
        const float off = std::fabs(wrap_radians(stepHeading - link_facing(g_map.heading, yaw_of(v.head.orientation))));
        facesAway = off > (facesAway ? 80.0f : 100.0f) * kPi / 180.0f;
    }
    for (int side = 0; side < 2; ++side) {
        // Link's left arm follows the left controller, his right arm the right one
        const auto& hand = g_input.hand[side];
        // (carrying something overhead: the game's carry pose. The carried actor is placed from his hands as drawn; with
        // them at the controllers a pot sat there, low and against his body, and broke on him when thrown, in first
        // person: reported in the headset. It is not drawn in first person, hides_held_object, so nothing is seen of it)
        const bool carrying = carries_overhead(link) && !carried_bomb(link);  // (a bomb: in the hand, the arms tracked)
        // gliding on the Deku Leaf (daPyProc_FAN_GLIDE_e): the arms at the controllers, each hand turned as the game's
        // glide pose turns it (below), so the leaf stands up over your hands, where you see it floating down (asked for
        // in the headset, 2026-11-03). It was the game's whole pose, the leaf above his head, out of the first-person
        // view; before that the hands took the controllers' turn and the leaf hung down at the left hand, unseen
        const bool gliding = ld32(link + kCurProc) == kProcLeafGlide;
        // hanging from a ledge (hang_proc): the game's pose, his hands on the edge. The shimmy moves him by how far his
        // hands' gap changes in the animation (procHangMove: mNormalSpeed from mLeftHandPos and mRightHandPos, read from
        // the hand joints right after this hook); at the controllers that gap hardly changed, and he could not move left
        // or right in first person (a player's video, 2026-11-09: "Can't Ledge Shimmy")
        const bool hanging = hang_proc(armProc);
        const bool tracked = haveHead && hand.active && !carrying && !facesAway && !hanging;
        {
            static bool wasTracked[2] = {};
            if (tracked != wasTracked[side]) {
                wasTracked[side] = tracked;
                const char* why = tracked ? "follows the controller again"
                                  : hanging ? "stops: Link hangs from a ledge (the game's pose: his hands move him along it)"
                                  : carrying ? "stops: Link carries something overhead (the game's carry pose)"
                                  : facesAway ? "stops: Link faces away from the head (walking backwards)"
                                  : !g_armsOk ? "stops: Link's arm joints did not check out"
                                  : !c.armIK ? "stops: arm IK is off"
                                  : !g_head.calibrated() ? "stops: not recentred yet"
                                  : !g_input.valid ? "stops: no controller input"
                                  : !play_view() ? "stops: not normal play (an event, cutscene or menu)"
                                  : g_thirdPerson.load() ? "stops: third-person view (motion controls off)"
                                  : !haveHead ? "stops: the headset's pose was lost for more than 2 s"
                                  : "stops: the controller's pose was lost for more than 2 s";
                // (with Link's action: on the boat it should never be "faces away", ship_ride_proc)
                LOG("[vr] %s arm %s (action 0x%X)", side ? "right" : "left", why, unsigned(armProc));
                if (!tracked && g_armsOk && c.armIK && g_head.calibrated() && g_input.valid && !play_view()) {
                    // which of normal play's conditions failed (aspect.cpp play_camera) and Link's action: a Quest 3
                    // report had both arms drop and Link stop while walking backwards, with no log of why
                    const uint32_t pl = play(), l = ld32(pl + 0x5B34);
                    LOG("[vr] %s arm: not normal play: Link's action 0x%X, event %u, controlled %d, flags 5BB2 %u 5BB3 %u "
                        "514C %u, 101F36CC %u, 101EA069 %u", side ? "right" : "left", l ? ld32(l + kCurProc) : 0u,
                        unsigned(ld8(pl + 0x5292)), int(l && ld32(pl + 0x5B2C) == l), unsigned(ld8(pl + 0x5BB2)),
                        unsigned(ld8(pl + 0x5BB3)), unsigned(ld8(pl + 0x5140 + 12)), unsigned(ld32(GD(0x101F36CC))),
                        unsigned(ld8(GD(0x101EA069))));
                }
            }
        }
        // (into and out of the Deku Leaf's glide over kGlideFadeSeconds: in 0.15 s the leaf popped from the hand to over
        // his head, reported in the headset)
        static float glideFade[2] = {};
        glideFade[side] = gliding ? kGlideFadeSeconds : std::max(0.0f, glideFade[side] - kStepSeconds);
        const float fadeSeconds = gliding || glideFade[side] > 0 ? kGlideFadeSeconds : 0.15f;
        g_armWeight[side] =
            std::clamp(g_armWeight[side] + (tracked ? 1.0f : -1.0f) * kStepSeconds / fadeSeconds, 0.0f, 1.0f);
        if (!g_armWeight[side] || !tracked) continue;  // tracking lost: the animation (no target to blend to)
        const int* j = kArmJoints[g_jointSet[side]];
        ArmJoints anim{read(j[0]), read(j[1]), read(j[2])};
        const Quat glideHandRot = anim.hand.rot;  // (gliding: the game's pose turns the hand to hold the leaf up)
        Joint clavicle = read(kClavicleJoints[g_jointSet[side]]), clavicleOut = clavicle;
        // The shoulder held steady against the run cycle: the animation lifts and drops Link's shoulders with every
        // stride, and an arm hanging from them carried the hand and the sword up and down while he ran (reported in
        // the headset; worst at full reach, where the hand is clamped to the reach around the shoulder). The upper
        // arm's root is kept at a settled place in Link's own frame (his position and heading): it holds through
        // the bob (measured: it rises and falls 7 to 16 units a second while running; a 9-unit dead zone) and follows a
        // real change of posture (crouching, climbing); the clavicle turns to put it there, at most about 34 degrees,
        // so the arm keeps its bones.
        if (g_claviclesOk) {
            const Vec3 fwd = heading_dir(stepHeading), right = heading_right(stepHeading);
            const Vec3 rel = anim.upper.pos - stepPos;
            const Vec3 local{dot(rel, right), rel.y, dot(rel, fwd)};
            auto& held = g_shoulderHold[side];
            if (!g_shoulderSeeded[side] || g_shoulderLink != link) {
                held.reset(local);
                g_shoulderSeeded[side] = true;
                if (side == 1) g_shoulderLink = link;
            }
            held.update(local, 9.0f, kStepSeconds, 0.3f);
            if (static const bool traceHand = [] { const char* e = std::getenv("WWHD_VR_TRACE_HAND"); return e && *e == '1'; }();
                traceHand && side == 1) {
                static float lo = 1e9f, hi = -1e9f;
                static int n = 0;
                lo = std::min(lo, local.y);
                hi = std::max(hi, local.y);
                if (++n == 30) {
                    LOG("[vr] shoulder trace: the animation's right shoulder rose and fell %.2f units in a second; held at %.2f",
                        double(hi - lo), double(held.value.y));
                    n = 0; lo = 1e9f; hi = -1e9f;
                }
            }
            const Vec3 want = stepPos + right * held.value.x + Vec3{0, held.value.y, 0} + fwd * held.value.z;
            const Vec3 C = clavicle.pos;
            Quat q = rotation_between(anim.upper.pos - C, want - C);
            const float angle = angle_between(q, Quat{});
            if (angle > 0.6f) q = slerp(Quat{}, q, 0.6f / angle);
            auto carry = [&](Joint& jt) {
                jt.rot = normalize(q * jt.rot);
                jt.pos = C + rotate(q, jt.pos - C);
            };
            if (finite(want) && angle > 1e-5f) {
                carry(anim.upper);
                carry(anim.lower);
                carry(anim.hand);
                clavicle.rot = normalize(q * clavicle.rot);
                clavicleOut = clavicle;
            }
            // what the turn cannot reach (the bob along the clavicle itself): the arm moved onto the held place, up
            // to 6 units (the shoulder's skin stretches that little, at the edge of the first-person view)
            const Vec3 rest = want - anim.upper.pos;
            if (finite(rest) && length(rest) < 6.0f) {
                anim.upper.pos = anim.upper.pos + rest;
                anim.lower.pos = anim.lower.pos + rest;
                anim.hand.pos = anim.hand.pos + rest;
            }
            if (static const bool traceShoulder = [] { const char* e = std::getenv("WWHD_VR_TRACE_HAND"); return e && *e == '1'; }();
                traceShoulder && side == 1) {
                static float worst = 0;
                static int m = 0;
                worst = std::max(worst, length(anim.upper.pos - want));
                if (++m == 30) {
                    LOG("[vr] shoulder trace: after the clavicle, at most %.2f units from its held place", double(worst));
                    m = 0; worst = 0;
                }
            }
        }
        const float linkArm = length(anim.lower.pos - anim.upper.pos) + length(anim.hand.pos - anim.lower.pos);
        // from the eyes, not the shoulders: the hand is drawn in the direction you see the controller in (the
        // camera's own position for this step: Link's eyes plus the head's lean)
        // (HeadController::step_head: this step's room-scale walk in the origin, stepPos has it too)
        const Vec3 eyes = g_head.step_head(v.head, stepPos + g_link.eyeOffset + Vec3{0, g_link.eyeHeight, 0},
                                           c.heightOffset, c.maxHeadOffset, world_scale()).position;
        const Vec3 offset = to_game_dir(g_map, hand.grip.position - v.head.position) * world_scale();
        const Vec3 seen = eye_wrist_target(eyes, offset, linkArm, c.realArmLength * world_scale());
        Vec3 wrist = keep_in_view(eyes, seen, anim.upper.pos, linkArm * 0.98f);
        // detached arms: the hand goes where you see the controller, not held to his shoulder's reach. Beyond it the
        // whole arm (upper arm, forearm, hand) moves out from the shoulder by what the reach lacks, at most one arm's
        // length; every bone keeps its length and the solver below poses it as always. Faded with the arm (tracking
        // lost: the animation's arm, where it was)
        const float detach = g_detachWeight * g_armWeight[side];
        if (detach > 0 && finite(seen)) {
            wrist = lerp(wrist, seen, detach);
            const Vec3 shift = detached_shift(anim.upper.pos, wrist, linkArm * 0.98f, linkArm) * detach;
            anim.upper.pos = anim.upper.pos + shift;
            anim.lower.pos = anim.lower.pos + shift;
            anim.hand.pos = anim.hand.pos + shift;
        }
        {
            // (diagnostics, the input summary) the controller's place beyond what his arm reaches: the hand cannot be
            // where you see the controller (Link's body away from you, or turned from where you face)
            const float beyond = length(eye_wrist_target(eyes, offset, linkArm, c.realArmLength * world_scale()) -
                                        anim.upper.pos) - linkArm;
            if (beyond > 2.0f) {
                ++g_outOfReach[side];
                g_maxOutOfReach[side] = std::max(g_maxOutOfReach[side], beyond);
            }
        }
        {
            auto& lag = g_lag[side];
            const Vec3 target = eye_wrist_target(eyes, offset, linkArm, c.realArmLength * world_scale());
            const uint64_t st = interp::logic_steps();
            const Quat rot = to_game_orientation(g_map, hand.aim.orientation);
            if (!lag.valid) lag.prev = lag.cur = target, lag.prevRot = lag.curRot = rot;
            else if (st != lag.step) { lag.prev = lag.cur; lag.cur = target; lag.prevRot = lag.curRot; lag.curRot = rot; }
            else lag.cur = target, lag.curRot = rot;
            lag.step = st;
            lag.k = linkArm / std::max(1e-3f, c.realArmLength * world_scale());
            lag.valid = true;
        }
        const Vec3 point = to_game_dir(g_map, rotate(hand.aim.orientation, {0, 0, -1}));
        // natural motion (vr_arm.h): the elbow between the animation's side and down / out / back from Link's body
        // (smoothed), the shoulder following extended reaches, the wrist within its range, the controller's roll
        ArmStyle style;
        // from your body's facing (you may have turned away from Link's heading) and where the hand is
        const Vec3 bodyForward = to_game_dir(g_map, rotate(yaw_quat(g_bodyYaw), {0, 0, -1}));
        Vec3 natural = natural_pole_body(bodyForward, side ? 1 : -1, wrist - anim.upper.pos, linkArm);
        // the sword hand: its hand is turned to the sword (below); the elbow goes where the forearm lines up with that
        // hand, so the wrist stays straight (asked for in the headset: the wrist bent up to hold the sword). Of the
        // elbows that reach the wrist, the one nearest the straight-wrist elbow (the wrist less the forearm along the
        // hand's line); the controller still decides the sword's direction.
        const bool swordHand = c.trackedSword && g_held.sword && g_held.gameHand >= 0 && side == dominant_side(c) &&
                               g_grip[g_jointSet[side]].valid;
        if (side == dominant_side(c) && !swordHand) g_swordRotValid = false;
        Quat swordWant;
        if (swordHand) {
            g_swordRot = held_sword_rotation(sword_base(hand), c);
            g_swordRotValid = true;
            swordWant = normalize(g_swordRot * conjugate(g_grip[g_jointSet[side]].rot));
            const Vec3 along = rotate(swordWant, g_wristStraight[g_jointSet[side]]);
            const Vec3 elbowWant = wrist - along * length(anim.hand.pos - anim.lower.pos);
            const Vec3 axis = normalize(wrist - anim.upper.pos);
            Vec3 toward = elbowWant - anim.upper.pos;
            toward = toward - axis * dot(toward, axis);
            if (finite(toward) && length(toward) > 1e-3f) natural = normalize(toward);
        }
        const float k = 1.0f - std::exp(-kStepSeconds / 0.1f);
        g_elbowPole[side] = normalize(lerp(g_elbowPole[side], natural, k), natural);
        style.pole = g_elbowPole[side];
        style.poleBlend = 1.0f;  // not the idle animation's elbow: it sways with his breathing while you hold still
        style.twist = controller_roll(hand.aim.orientation);
        // detached arms: the hand bends and twists as far as the controller (80 and 100 degrees otherwise)
        style.wristLimit = detached_limit(style.wristLimit, kPi, detach);
        style.twistLimit = detached_limit(style.twistLimit, kPi, detach);
        // an empty hand: its roll the controller's from level, not added to the animation's (which turned with his body:
        // the hands rolled over when he ran left or right, reported with a video). A held item keeps the game's hold
        if (g_held.gameHand != kArmJoints[g_jointSet[side]][2]) {
            style.levelUp = g_handLevelUp[g_jointSet[side]];
            // (2026-11-10: held rigidly to the controller instead; the roll from level spun the hanging hands as the
            // player walked round, there being no level with the hand pointing down)
            if (g_ctrlToHandOk[g_jointSet[side]]) {
                style.rigid = true;
                style.ctrl = to_game_orientation(g_map, hand.aim.orientation);
                style.ctrlToHand = g_ctrlToHand[g_jointSet[side]];
            }
        }
        ArmJoints posed = pose_arm_natural(anim, g_claviclesOk ? &clavicle : nullptr, wrist, point, style,
                                           g_armWeight[side], &clavicleOut, &g_hingeLocal[side]);
        if (!finite(posed.hand.pos) || !finite(posed.lower.pos)) continue;
        // the sword hand holds the sword: turned so the game's grip holds while the blade follows this controller
        // (the wrist stays where the IK put it; the controller still decides the hand's orientation, through the sword)
        if (swordHand) {
            posed.hand.rot = slerp(posed.hand.rot, swordWant, g_armWeight[side]);
            // the wrist in line with the forearm (vr.swordWristStraight, 1 by default): the hand turned down onto the
            // forearm's line, keeping its twist; the sword, attached to the hand, turns with it (asked for in the
            // headset: the wrist bent up to hold the sword). The elbow above already lines the forearm up as far as
            // the arm allows; this takes the rest. The swing's detection still reads the controller, its collision the
            // drawn blade.
            // No longer (2026-11-03): straightened, the hand turned off the handle the sword is placed from, by as much as
            // it straightened (reported with a video: "hand still disconnects from handle"). The hand and the sword are
            // one piece now (docs/VR_HELD_ITEMS.md, "One piece"): the hand is exactly on the grip; the elbow above still
            // lines the forearm up as far as the arm allows
            // (diagnostics, WWHD_VR_TRACE_LAG=1) how far the wrist bends off the forearm's line, every 2 s
            if (static const bool traceBend = [] { const char* e = std::getenv("WWHD_VR_TRACE_LAG"); return e && *e == '1'; }();
                traceBend) {
                static double sum = 0, worst = 0;
                static int n = 0;
                const Vec3 fore = normalize(posed.hand.pos - posed.lower.pos);
                const Vec3 handLine = rotate(posed.hand.rot, g_wristStraight[g_jointSet[side]]);
                const double bend = std::acos(std::clamp(double(dot(fore, handLine)), -1.0, 1.0)) * 180.0 / kPi;
                sum += bend;
                worst = std::max(worst, bend);
                if (++n == 60) {
                    LOG("[vr] sword wrist: bent %.0f degrees off the forearm on average, %.0f at most", sum / n, worst);
                    sum = worst = 0;
                    n = 0;
                }
            }
        }
        // the off hand, empty, the same way: its wrist in line with the forearm, its twist kept (asked for in the headset: the
        // left hand tilted up off the arm). Not while it holds something turned by its controller: the shield, or an item
        // held and aimed with it (the bow, the hookshot, the grappling hook: aim_side)
        // (not for a hand held rigidly to its controller, style.rigid: the forearm's line follows the elbow, which follows
        // the body's turn, and the left hand twisted with every turn of the body; a player's video, 2026-11-10, "the left
        // hand still rotates when turning left or right or moving in a circle")
        if (!style.rigid && side != dominant_side(c) && c.swordWristStraight > 0 && !g_held.shieldOnHand &&
            aim_side(c, ld16(link + 0x69B0)) != side) {
            const Vec3 fore = normalize(posed.hand.pos - posed.lower.pos);
            const Vec3 handLine = rotate(posed.hand.rot, g_wristStraight[g_jointSet[side]]);
            const Quat straighten = slerp(Quat{}, rotation_between(handLine, fore),
                                          std::clamp(c.swordWristStraight, 0.0f, 1.0f) * g_armWeight[side]);
            if (finite(fore) && finite(handLine)) posed.hand.rot = normalize(straighten * posed.hand.rot);
        }
        // the shield hand on the shield's handle, as the sword hand is on the sword's (shield_hand)
        if (side != dominant_side(c)) {
            Quat onHandle;
            if (shield_hand(posed.hand, c, dominant_side(c), onHandle))
                posed.hand.rot = normalize(slerp(posed.hand.rot, onHandle, g_armWeight[side]));
        }
        // gliding: the hand where your controller is, turned as the game's glide pose turns it, so the leaf it holds
        // stands up over it (the controller's own turn hung it down, out of view)
        if (gliding) posed.hand.rot = normalize(slerp(posed.hand.rot, glideHandRot, g_armWeight[side]));
        if (g_claviclesOk) write(kClavicleJoints[g_jointSet[side]], clavicleOut);
        write(j[0], posed.upper);
        write(j[1], posed.lower);
        write(j[2], posed.hand);
        g_lag[side].handRot = posed.hand.rot;  // (the hand each pass is turned from: FrameHand::handNow)
        g_lag[side].haveHand = finite(posed.hand.pos);
        // diagnostics (WWHD_VR_TRACE_HAND=1): the posed hand against the eyes, per step, with Link's speed
        if (static const bool traceHand = [] { const char* e = std::getenv("WWHD_VR_TRACE_HAND"); return e && *e == '1'; }();
            traceHand && side == 1) {
            static Vec3 lastRel, lastPos;
            static double jitter = 0, speed = 0, wristJitter = 0;
            static Vec3 lastWrist;
            static int n = 0;
            const Vec3 rel = posed.hand.pos - eyes, wr = wrist - eyes;
            if (n) {
                jitter += length(rel - lastRel);
                wristJitter += length(wr - lastWrist);
                speed += length(g_link.pos - lastPos);
            }
            lastRel = rel; lastWrist = wr; lastPos = g_link.pos;
            if (++n == 31) {
                LOG("[vr] hand trace: per step, the hand moved %.2f units against the eyes (its target %.2f) while Link "
                    "moved %.2f", jitter / 30, wristJitter / 30, speed / 30);
                n = 1; jitter = speed = wristJitter = 0;
            }
        }
        changed = true;
        if (!g_handsLogged[side]) {
            g_handsLogged[side] = true;
            auto below = [](Vec3 d) { return std::atan2(-d.y, std::sqrt(d.x * d.x + d.z * d.z)) * 180.0f / kPi; };
            LOG("[vr] first person %s hand: controller %.0f degrees below the eyes, Link's hand %.0f (%.0f units away)",
                side ? "right" : "left", double(below(offset)), double(below(posed.hand.pos - eyes)),
                double(length(posed.hand.pos - eyes)));
        }
    }
    if (changed) st16(block + kMtxBlockFlags, uint16_t(ld16(block + kMtxBlockFlags) | kMatricesChanged));
}

// ---- Link's first-person presentation
namespace {
uint32_t g_statusAddr = 0, g_statusSaved = 0;
// Link's hands as drawn (his logic never sees them). His hands are meshes of a hands model that his draw picks per
// hand (daPy_lk_c::setDrawHandModel, decomp d_a_player_main_01.cpp): fixed ones for the held item (mEquipItem +0x69B0:
// the sword 0x103 shows the left fist 3 and, with a shield, the right hand around its grip 8; 0x100 is nothing), else
// mLeftHandIdx (+0x68DC: 0 his body's open hand, 3 the fist ...) and mRightHandIdx (+0x68DD: 0 open, 8 closed around
// a grip, 9 the fist around a stick ...). The game has the closed right hand; nothing is drawn anew.
// For his draw: the item reads as nothing where the game's fixed sword hands would be wrong (the sword moved to his
// right hand), and the hand indices say which hand is closed (the sword's hand, a pressed grip). Set from his draw
// until just before his next execute (link_executing), not only during his draw: the frames between logic steps are
// drawn from what the game built without calling his draw again (restoring at its end made a hand flicker).
// (Earlier this wrote +0x69B1 and +0x69B3: the item's low byte and the next field, not hand shapes; an empty right
// hand stayed open, reported in the headset.)
constexpr uint32_t kEquipItem = 0x69B0, kLeftHandIdx = 0x68DC, kRightHandIdx = 0x68DD;
constexpr uint16_t kItemNone = 0x100;
constexpr uint8_t kLeftClosed = 3, kRightClosed = 8, kRightFist = 9;  // (9: the fist round a stick's handle)
uint32_t g_handsLink = 0;          // the Link whose values below are saved (0: nothing changed)
uint16_t g_savedItem = 0;
// the item Link holds as the game has it (mEquipItem), also between his draw and his next execute, when the draw may
// have left another value there for his hand shapes (apply_hands)
uint16_t game_item(uint32_t link) { return link && g_handsLink == link ? g_savedItem : ld16(link + kEquipItem); }
uint8_t g_savedHands[2] = {};
int g_drawItemHand = -1;           // this pass: the hand joint another held item was moved to (closed around it), else -1
int g_drawSwordHand = -1;          // this pass: the hand joint the sword was moved to (another than the game's), else -1
int g_drawShieldHand = -1;         // this pass: the hand joint the shield was moved to (another than the game's), else -1
// Link's head in first person (docs/VR_FIRST_PERSON_CAMERA.md). The camera sits at Link's eyes, inside his head: his
// face, hair, hat and bangs would fill the view. Two layers hide them from the gameplay view:
// - the game's own close-camera presentation (camera attention status 0x20) for its material visibility;
// - the head's joints themselves: the body model's head joints (15 and the hat / hair / face joints after it, up to
//   the legs at 0x20, those near the head) and the separate hair and bangs models (mpKatsuraModel +0x44C,
//   mpYamuModel +0x450) are collapsed onto the neck (joint 14), so every vertex skinned to them shrinks into the neck
//   opening (a weight shared with the neck stays between the two; nothing stretches away). The neck itself is
//   collapsed onto its base as well: looking down, its skin was in view below the eyes.
// The collapse is not undone after the draw: the game's update_ubo job thread copies the matrices for the GPU while
// the main thread draws on, so putting them back at the end of Link's draw raced with that copy (the hair stayed in
// view in the first headset test). Link's next execute recomputes them from the animation (J3DModel::calc), so the
// skeleton, the animation and everything read from it are untouched; outside normal play (cutscenes, events, menus)
// nothing is collapsed, so the head is back from the first frame of an event.
constexpr uint32_t kHairModels[] = {0x44C, 0x450};
// the head's joints are 15 to 28 (hat, hair, face); 29 and 30 are his waist and 31 to 40 his legs (measured with
// WWHD_VR_TRACE_SKELETON: the waist and the left thigh are within kHeadRadius of the head joint, and collapsing them
// onto the neck hid his hips and pulled his left leg up: you could not see his whole body). All of 15 to 28 are hidden,
// however far the wind blows the hat (hide_head); kHeadRadius now only checks the head is at the neck
constexpr int kNeckJoint = 14, kHeadJoint = 15, kFirstLegJoint = 29;
constexpr float kHeadRadius = 45.0f;  // game units around the head joint that count as the head (hat tip included)
uint32_t g_headLoggedLink = 0;

uint32_t model_matrices(uint32_t model, uint16_t* count, uint32_t* block = nullptr) {
    const uint32_t b = model ? ld32(model + kMtxBlock) : 0;
    const uint32_t mtx = b ? ld32(b + kMtxBlockMatrices) : 0;
    *count = b ? ld16(b + 0x2C) : 0;
    if (block) *block = b;
    return *count && *count <= 64 ? mtx : 0;
}
Vec3 joint_position(uint32_t mtx, int j) {
    return {float(ldf32(mtx + j * 48 + 12)), float(ldf32(mtx + j * 48 + 28)), float(ldf32(mtx + j * 48 + 44))};
}
// a joint matrix that maps everything onto `point`
void collapse_joint(uint32_t mtx, int j, Vec3 point) {
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k) st32(mtx + j * 48 + r * 16 + 4 * k, 0);
    }
    stf32(mtx + j * 48 + 12, point.x);
    stf32(mtx + j * 48 + 28, point.y);
    stf32(mtx + j * 48 + 44, point.z);
}
void mark_changed(uint32_t block) {
    if (block) st16(block + kMtxBlockFlags, uint16_t(ld16(block + kMtxBlockFlags) | kMatricesChanged));
}
void hide_head(uint32_t link) {
    uint16_t count = 0;
    uint32_t block = 0;
    const uint32_t body = model_matrices(ld32(link + kBodyModel), &count, &block);
    if (!body || count <= kHeadJoint) return;
    const Vec3 neck = joint_position(body, kNeckJoint), head = joint_position(body, kHeadJoint);
    if (!finite(neck) || !finite(head) || length(head - neck) > kHeadRadius) return;
    // the neck too, onto its own base: its skin (drawn by joint 14 alone) was a patch at the bottom of the view when
    // looking down; the collar, shared with the chest, only shrinks towards it (the arms, 5 to 12, come before it)
    collapse_joint(body, kNeckJoint, neck);
    int collapsed = 1;
    // every head joint (15 to 28: the hat, the hair, the face), wherever it is: the hat's joints blown by the wind reach
    // past kHeadRadius from the head, and the hat was then drawn into the view (reported in the headset). The waist
    // and legs, which kHeadRadius kept out, are 29 on and never in this range
    for (int j = kHeadJoint; j < std::min<int>(count, kFirstLegJoint); ++j) {
        collapse_joint(body, j, neck);
        ++collapsed;
    }
    mark_changed(block);
    for (uint32_t off : kHairModels) {
        uint16_t n = 0;
        uint32_t b = 0;
        const uint32_t m = model_matrices(ld32(link + off), &n, &b);
        if (!m) continue;
        for (int j = 0; j < n; ++j) collapse_joint(m, j, neck);
        mark_changed(b);
    }
    if (g_headLoggedLink != link) {
        g_headLoggedLink = link;
        LOG("[vr] first person: Link's head and neck hidden (%d body joints from %d, hair and bangs) at the neck's base",
            collapsed, kNeckJoint);
    }
}
}  // namespace

// Link's hands for this pass: the sword's hand closed around it when the sword was moved to his other hand (the
// hand it left open), and a pressed grip closes the hand its controller drives when that hand holds nothing (a hand
// holding an item keeps the game's grip for it)
void apply_hands(uint32_t link) {
    static const bool testGrip = [] { const char* e = std::getenv("WWHD_VR_TEST_GRIP"); return e && *e == '1'; }();
    bool grip[2] = {};  // Link's left, right hand
    for (int side = 0; side < 2; ++side)
        // (closed by default in first person: no grip to press, asked for with a video of another VR game, the fists
        // fixed on the sword and the shield)
        if ((kFistsClosed || g_gripLatch[side].closed || testGrip) && !g_thirdPerson.load()) grip[g_jointSet[side]] = true;
    const uint16_t item = ld16(link + kEquipItem);
    const int swordHand = g_drawSwordHand, itemHand = g_drawItemHand, shieldHand = g_drawShieldHand;
    g_drawSwordHand = g_drawItemHand = g_drawShieldHand = -1;
    // the hand the shield was moved to closes round its grip, as the game closes its own shield hand with the sword out
    // (setDrawHandModel: the sword with a shield, the right hand 8); open, the hand hung beside the shield (reported in
    // the headset)
    const bool shieldLeft = shieldHand == kArmJoints[0][2], shieldRight = shieldHand == kArmJoints[1][2];
    // (nothing in his hands in first person: on to the open hands below, whatever shapes his animation left)
    const bool emptyHands = item == kItemNone && !g_thirdPerson.load();
    if (swordHand < 0 && itemHand < 0 && shieldHand < 0 && !emptyHands) {
        // nothing to change in this pass: the game's own values (as saved, if an earlier pass of this step changed them)
        if (g_handsLink == link) {
            st16(link + kEquipItem, g_savedItem);
            st8(link + kLeftHandIdx, g_savedHands[0]);
            st8(link + kRightHandIdx, g_savedHands[1]);
        }
        return;
    }
    if (g_handsLink != link) {  // the game's values of this step, put back by link_executing
        g_handsLink = link;
        g_savedItem = item;
        g_savedHands[0] = ld8(link + kLeftHandIdx);
        g_savedHands[1] = ld8(link + kRightHandIdx);
    }
    uint8_t left = g_savedHands[0], right = g_savedHands[1];
    if (swordHand >= 0) {
        // not the game's fixed sword hands (they are on the other side now). Not for the hammer: its own shapes close
        // both hands (setDrawHandModel), and the swing's press read the blanked item between this draw and his next
        // execute, so a hammer swing pressed B and cut with the sword (seen in the headset)
        if (g_savedItem != kItemSkullHammer) st16(link + kEquipItem, kItemNone);
        // the right hand as the game closes it round a stick's handle (9, setDrawHandModel for daPyItem_BOKO_e): its
        // shape around the shield's grip (8) left the fingers open beside the sword's handle (reported in the headset)
        const bool toRight = swordHand == kArmJoints[1][2];
        left = toRight ? (grip[0] || shieldLeft ? kLeftClosed : 0) : kLeftClosed;
        right = toRight ? kRightFist : (grip[1] || shieldRight ? kRightClosed : 0);
    } else {
        if (grip[0] || shieldLeft) left = kLeftClosed;
        else if (emptyHands) left = 0;  // (his body's open hand)
        if (grip[1] || shieldRight) right = kRightClosed;
        else if (emptyHands) right = 0;
        // another item moved to this hand (the bow, the grappling hook in the off hand): closed around it (the game's
        // shapes for it still apply where it fixes them: the bow's while drawn, the grappling hook's)
        if (itemHand == kArmJoints[0][2]) left = kLeftClosed;
        else if (itemHand == kArmJoints[1][2]) right = kRightClosed;
    }
    st8(link + kLeftHandIdx, left);
    st8(link + kRightHandIdx, right);
}

// `count` 3x4 matrices at `mtx` moved rigidly: the point `from` to `at`, turned by `delta`
void move_matrices(uint32_t mtx, int count, Vec3 from, Vec3 at, Quat delta) {
    auto row = [&](int j, int r) { return Vec3{float(ldf32(mtx + j * 48 + r * 16)), float(ldf32(mtx + j * 48 + r * 16 + 4)),
                                               float(ldf32(mtx + j * 48 + r * 16 + 8))}; };
    auto trans = [&](int j) { return Vec3{float(ldf32(mtx + j * 48 + 12)), float(ldf32(mtx + j * 48 + 28)),
                                          float(ldf32(mtx + j * 48 + 44))}; };
    for (int j = 0; j < count; ++j) {
        const Vec3 cols[3] = {Vec3{row(j, 0).x, row(j, 1).x, row(j, 2).x}, Vec3{row(j, 0).y, row(j, 1).y, row(j, 2).y},
                              Vec3{row(j, 0).z, row(j, 1).z, row(j, 2).z}};
        const Vec3 t = at + rotate(delta, trans(j) - from);
        Vec3 n[3];
        for (int k = 0; k < 3; ++k) n[k] = rotate(delta, cols[k]);
        auto component = [](Vec3 v, int i) { return i == 0 ? v.x : i == 1 ? v.y : v.z; };
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k) stf32(mtx + j * 48 + r * 16 + 4 * k, component(n[k], r));
        stf32(mtx + j * 48 + 12, t.x);
        stf32(mtx + j * 48 + 28, t.y);
        stf32(mtx + j * 48 + 44, t.z);
    }
}
// a model's joints moved rigidly: its root `from` to `at`, turned by `delta`; marked changed for the GPU copy
void move_model(uint32_t model, Vec3 from, Vec3 at, Quat delta) {
    uint16_t count = 0;
    uint32_t block = 0;
    const uint32_t mtx = matrices_of(model, count, &block);
    if (!mtx || count > 32) return;
    move_matrices(mtx, count, from, at, delta);
    st16(block + kMtxBlockFlags, uint16_t(ld16(block + kMtxBlockFlags) | kMatricesChanged));
}
// a model placed with its root at (rot, pos): moved from wherever it is now, so placing it again in a later pass of
// the same step gives the same result
void place_model(uint32_t model, Quat rot, Vec3 pos) {
    uint16_t count = 0;
    const uint32_t mtx = model ? matrices_of(model, count) : 0;
    if (!mtx || count > 32 || !finite(pos)) return;
    const Joint cur = read_joint(mtx, 0);
    if (!finite(cur.pos)) return;
    move_model(model, cur.pos, pos, normalize(rot * conjugate(cur.rot)));
}

// where the held model was placed by Link's last draw: a picked-up weapon is its own actor and is drawn by its own
// draw, which may come before Link's in a frame (then his placement came too late for it: the stick floated where the
// game holds it, away from the tracked hand)
struct HeldTarget { uint32_t model = 0; Quat rot; Vec3 at; bool valid = false; } g_heldTarget;
uint64_t g_linkDrawnFrame = ~0ull;

// The hookshot's tip (its barrel's end, joint kHookshotTip of the held model): Link's draw sets it to the hookshot
// actor's top matrix (daPy_lk_c::draw, setAnmMtx(HOOKSHOT_JNT_HSHOTTIP, hookshot->getMtxTop())), after the held model
// was moved to the tracked hand (link_draw_begin). That matrix comes from the game's own hand, where the aiming
// animation holds it before the view: the barrel stayed there, low in the view, while the hand moved (reported with a
// video: the hookshot arm stuck in place). Resting in the barrel, the tip is put back rigid with the hookshot in the hand
// (its place against the model's root as the model was calculated, before the draw); fired, it is the flying hook and
// stays the game's
constexpr int kHookshotTip = 4;                    // HOOKSHOT_JNT_HSHOTTIP_e
constexpr uint32_t kHookshotTopMtx = 0xD778;       // daHookshot_c's top matrix (getMtxTop)
constexpr float kHookshotRestUnits = 50.0f;        // the actor's top this near the tip's rest: in the barrel (resting it reads
                                                  // 0 or 21.9 on alternate draws; fired, 100 and more)
struct HookshotTip { bool rest = false; Joint local; } g_hookshotTip;
void hookshot_tip_begin(uint32_t link) {
    g_hookshotTip.rest = false;
    if (!first_person_play() || game_item(link) != kItemHookshot) return;
    const uint32_t actor = ld32(link + kKeptEquipActor);
    uint16_t n = 0;
    const uint32_t mtx = matrices_of(ld32(link + kEquipModel), n);
    if (!actor || !mtx || n <= kHookshotTip) return;
    const Joint root = read_joint(mtx, 0), tip = read_joint(mtx, kHookshotTip);
    float top[12];
    for (int i = 0; i < 12; ++i) top[i] = float(ldf32(actor + kHookshotTopMtx + i * 4));
    const Vec3 actorTip = joint_from_matrix(top).pos;
    if (!finite(root.pos) || !finite(tip.pos) || !finite(actorTip)) return;
    g_hookshotTip.local.rot = normalize(conjugate(root.rot) * tip.rot);
    g_hookshotTip.local.scale = tip.scale;
    g_hookshotTip.local.pos = rotate(conjugate(root.rot), tip.pos - root.pos);
    g_hookshotTip.rest = length(actorTip - tip.pos) < kHookshotRestUnits;
    static int logged = -1;
    if (int(g_hookshotTip.rest) != logged) {
        logged = int(g_hookshotTip.rest);
        LOG("[vr] hookshot: its tip %s (the actor's top %.1f units from the barrel)",
            g_hookshotTip.rest ? "in the barrel, kept on the hookshot in the hand" : "out: the game's (the flying hook)",
            double(length(actorTip - tip.pos)));
    }
}
void hookshot_tip_end(uint32_t link) {
    if (!g_hookshotTip.rest) return;
    g_hookshotTip.rest = false;
    uint16_t n = 0;
    uint32_t block = 0;
    const uint32_t mtx = matrices_of(ld32(link + kEquipModel), n, &block);
    if (!mtx || n <= kHookshotTip) return;
    const Joint root = read_joint(mtx, 0);
    if (!finite(root.pos)) return;
    Joint tip = g_hookshotTip.local;
    tip.rot = normalize(root.rot * g_hookshotTip.local.rot);
    tip.pos = root.pos + rotate(root.rot, g_hookshotTip.local.pos);
    float m[12];
    matrix_from_joint(tip, m);
    for (int i = 0; i < 12; ++i) stf32(mtx + kHookshotTip * 48 + i * 4, m[i]);
    st16(block + kMtxBlockFlags, uint16_t(ld16(block + kMtxBlockFlags) | kMatricesChanged));
}

// The shield in the off hand turned by its controller (docs/VR_HELD_ITEMS.md, "The shield's orientation"): flush with
// the body, its face where the controller points (levelled, tilted with the wrist), its top up, its handle at the hand
// `to`, moved by vr.shieldOffset*. One rule for the step's placement and each pass's (frame_correct_pose): placed by it
// at the step and from the posed hand between steps, the two disagreed whenever the wrist rolled and the shield shook
// between them (reported in the headset: "vibrating", "fighting for direction"). False: no controller or axes yet
bool shield_by_controller(const Joint& to, const Config& c, int d, Quat& rot, Vec3& pos) {
    const ShieldAxes& axes = g_shield.axes;
    const auto& ctrl = g_input.hand[1 - d];
    if (!axes.valid() || !g_input.valid || !ctrl.active || !(g_armWeight[1 - d] > 0) || !finite(to.pos)) return false;
    rot = shield_rotation(axes, to_game_dir(g_map, shield_face_tilted(ctrl.aim.orientation)), to_game_dir(g_map, Vec3{0, 1, 0}));
    const Vec3 handInShield = mirror_across_face(rotate(conjugate(g_held.shieldRot), g_held.shieldPos * -1.0f), axes);
    pos = to.pos - rotate(rot, handInShield);
    if (!c.leftHanded) {  // right-handed play: where the offsets were set (tilted with the face)
        const Vec3 face = shield_face_tilted(ctrl.aim.orientation);
        const Vec3 up = normalize(Vec3{0, 1, 0} - face * face.y), right = cross(face, up);
        pos = pos + to_game_dir(g_map, right * c.shieldOffsetX + up * c.shieldOffsetY - face * c.shieldOffsetZ) * world_scale();
    }
    return finite(pos);
}
// The shield hand locked to the shield's handle (asked for in the headset, as the sword hand is to the sword): its turn
// is the shield's (shield_by_controller) with the game's own grip on the handle (his right hand's attachment, mirrored
// onto the hand holding it, as the shield is: mirror_held). False: the shield is not in the other hand, or no controller
bool shield_hand(const Joint& to, const Config& c, int d, Quat& handRot) {
    if (!g_held.shieldOnHand || kArmJoints[g_jointSet[1 - d]][2] == kArmJoints[1][2]) return false;
    Quat shieldRot;
    Vec3 shieldPos;
    if (!shield_by_controller(to, c, d, shieldRot, shieldPos)) return false;
    Quat grip = g_held.shieldRot;
    Vec3 gripPos = g_held.shieldPos;
    const int flip = g_shield.axes.valid() ? g_shield.axes.side() : g_held.shieldFlip;
    if (g_mirrorKnown) mirror_held(g_held.shieldRot, g_held.shieldPos, g_mirrorSign, flip, grip, gripPos);
    handRot = normalize(shieldRot * conjugate(grip));
    return true;
}

void link_draw_begin(uint32_t link) {
    GameWorkTimer workTimer(GameWork::Draw);  // (docs/VR_PERFORMANCE.md: the VR work on the game thread)
    g_statusAddr = 0;
    if (!running() || !link) return;
    hookshot_tip_begin(link);  // (before the held model is moved: the tip against it as the game calculated them)
    const Config c = config();
    if (!play_view()) return;
    // after everything below (the sword's shape swap included), whichever way it returns
    struct GrippedHands {
        uint32_t link;
        ~GrippedHands() { apply_hands(link); }
    } grippedHands{link};
    // the game's own close-camera presentation: no head, hair or hat in front of the eyes; arms and legs stay
    const int32_t idx = int32_t(ld32(link + kCameraInfoIdx));
    if ((!c.showBody || c.hideBody) && idx == 0 && !g_thirdPerson.load()) {  // third person: his whole model, head included
        g_statusAddr = play() + kPlayCameraInfo + uint32_t(idx) * kCameraInfoSize + kAttentionStatus;
        g_statusSaved = ld32(g_statusAddr);
        st32(g_statusAddr, g_statusSaved | kCloseCameraStatus);
        hide_head(link);
    }
    trace_held(link, "draw");
    trace_hand_joint(link, true);
    if (!c.trackedSword || g_thirdPerson.load()) return;  // third person: held items where the game puts them
    // What Link holds, drawn in the hands the player uses (dominant_side), moved rigidly every pass from where the
    // game put it. The game holds the sword in Link's left hand and the shield in his right (he is left-handed):
    // - the sword (blade: mpEquipItemModel; hilt: a model of its own on the same hand) in the sword hand: the grip
    //   in Link's hand as the arm IK posed it, the blade along that controller (the same blade the swing's
    //   collision uses, link_executed); the hilt moves with the blade, so they stay one sword;
    // - another held item (telescope, ...): the game's own attachment, moved to the dominant hand when that is not
    //   the game's;
    // - right-handed, the shield moves to the left hand (the sword's hand and the shield's swap);
    // - nothing in a hand (a sheathed sword, a shield on the back, an item being put away): as the game draws it.
    std::lock_guard<std::mutex> lock(g_mutex);
    for (HandLock& l : g_locks) l.valid = false;  // (taken again below from this step's placement)
    const int d = dominant_side(c);
    uint16_t bodyCount = 0;
    const uint32_t body = matrices_of(ld32(link + kBodyModel), bodyCount);
    if (!body || bodyCount <= 12 || !g_head.calibrated()) return;
    const int swordHand = kArmJoints[g_jointSet[d]][2], offHand = kArmJoints[g_jointSet[1 - d]][2];
    auto move = move_model;
    auto place = place_model;
    // the shield: from the game's shield hand (Link's right: he is left-handed) to the player's off hand (right-handed:
    // Link's left). His hands' frames are mirror images, so its attachment is mirrored onto the other hand as the
    // sword's grip is (mirror_held): applied unchanged, the right hand's attachment held it upside down in the left
    // (reported in the headset). It is flipped on its axis across its own face, so its top stays up and its crest out
    const int gameShieldHand = kArmJoints[1][2];
    if (offHand != gameShieldHand && g_held.shieldOnHand) {
        const Joint to = read_joint(body, offHand);
        const auto& offController = g_input.hand[1 - d];
        const ShieldAxes& axes = g_shield.axes;
        Quat shieldWorld;
        // turned by the off-hand controller alone (shield_by_controller: flush with the body, its face where the
        // controller points, top up, the handle at the hand; out of the back of the hand it stood sideways, reported in
        // the headset). Not by Link's posed hand: its turn keeps the animation's (the game swings that hand with the
        // sword's attacks)
        if (!shield_by_controller(to, c, d, shieldWorld, g_held.shieldPlaced)) {
            // (no controller, or its axes not learnt yet: the game's attachment mirrored onto the posed hand)
            Quat rot = g_held.shieldRot;
            Vec3 pos = g_held.shieldPos;
            const int flip = axes.valid() ? axes.side() : g_held.shieldFlip;
            // (his rig's mirror not measured: as before, unmirrored, rather than left in the sword's hand)
            if (g_mirrorKnown) mirror_held(g_held.shieldRot, g_held.shieldPos, g_mirrorSign, flip, rot, pos);
            shieldWorld = normalize(to.rot * rot);
            g_held.shieldPlaced = to.pos + rotate(to.rot, pos);
        }
        // one piece with the hand (docs/VR_HELD_ITEMS.md, "One piece"): its place in the hand is taken once, when the hand
        // follows its controller fully, then kept: the shield turns with the hand, 1 to 1, instead of being turned by the
        // controller beside a hand posed apart from it (the hand came loose from it, reported with a video)
        static struct { bool valid = false; uint32_t model = 0; Quat rot; Vec3 pos; float ox = 0, oy = 0, oz = 0; } held;
        const uint32_t shieldModel = ld32(link + kShieldModel);
        if (held.valid && (held.model != shieldModel || held.ox != c.shieldOffsetX || held.oy != c.shieldOffsetY ||
                           held.oz != c.shieldOffsetZ))
            held.valid = false;
        // (off: kept, it rolled with the wrist, its point no longer down, reported in the headset; the controller's
        // levelled orientation each step again, and each pass from the hand as drawn: g_locks)
        if (kShieldPlaceKept && !held.valid && axes.valid() && g_input.valid && offController.active &&
            g_armWeight[1 - d] >= 1.0f && finite(to.pos) && finite(g_held.shieldPlaced)) {
            held = {true, shieldModel, normalize(conjugate(to.rot) * shieldWorld),
                    rotate(conjugate(to.rot), g_held.shieldPlaced - to.pos), c.shieldOffsetX, c.shieldOffsetY, c.shieldOffsetZ};
            LOG("[vr] shield: one piece with the hand from here (its place in the hand kept)");
        }
        if (held.valid) {
            shieldWorld = normalize(to.rot * held.rot);
            g_held.shieldPlaced = to.pos + rotate(to.rot, held.pos);
        }
        place(shieldModel, shieldWorld, g_held.shieldPlaced);
        lock_to_hand(kLockShield, shieldModel, body, offHand);
        g_drawShieldHand = offHand;  // its hand closes round its grip (apply_hands)
        static int loggedWay = -1;
        const int way = axes.valid() && g_input.valid && offController.active && g_armWeight[1 - d] > 0 ? 1 : 0;
        if (way != loggedWay) {
            loggedWay = way;
            LOG("[vr] shield: in Link's %s hand, %s", offHand == kArmJoints[0][2] ? "left" : "right",
                way ? "turned by its controller (flush with the body: face where it points, top up)"
                    : "the game's attachment mirrored onto the posed hand");
        }
    }
    const uint32_t model = held_model(link);
    g_heldTarget.valid = false;
    if (!model || model != g_held.model || g_held.gameHand < 0) return;
    // the hand it is drawn in: the sword hand, or the off hand for the bow and the grappling hook (off_hand_item)
    const int itemHand = g_held.sword ? swordHand : kArmJoints[g_jointSet[aim_side(c, game_item(link))]][2];
    const Joint hand = read_joint(body, itemHand);
    if (!finite(hand.pos)) return;
    Quat rot;
    Vec3 at;
    const Grip& grip = g_grip[swordHand == kArmJoints[1][2] ? 1 : 0];
    if (g_held.sword && grip.valid && c.armIK) {
        // at the posed hand's grip, turned as the controller holds it this step (g_swordRot; the hand may be turned
        // straight with the forearm, the sword keeps pointing where the controller points)
        rot = g_swordRotValid ? g_swordRot : normalize(hand.rot * grip.rot);
        at = hand.pos + rotate(hand.rot, grip.pos);
        // a picked-up weapon: its handle at the hand (weapon_target)
        if (game_item(link) == kItemEnemyWeapon && weapon_target(link, c, body, rot, at)) {
            // its base in the frame of the hand holding it (g_weaponGrip: each pass then places it from that hand), taken
            // once a step, at its first draw. Taken at every draw it set the controller's pose of that pass against the
            // hand of the step (his skeleton is the step's until the next one), and the pass then placed the stick from
            // the hand as drawn, which follows the controller already: the hand's move since the step counted twice,
            // the stick overshot the hand and came back at each step (reported in the headset: it wobbled and was
            // displaced now and then)
            static uint64_t gripStep = ~0ull;
            const uint64_t step = interp::logic_steps();
            if (step != gripStep || !g_weaponGrip.valid) {
                gripStep = step;
                const Joint held = read_joint(body, swordHand);
                const Quat gripRot = normalize(conjugate(held.rot) * rot);
                const Vec3 gripPos = rotate(conjugate(held.rot), at - held.pos);
                g_weaponGrip.valid = finite(held.pos);
                g_weaponGrip.set = swordHand == kArmJoints[1][2] ? 1 : 0;
                g_weaponGrip.rot = gripRot;
                g_weaponGrip.pos = gripPos;
            }
        }
    } else if (g_held.sword && g_held.haveLast && g_input.valid && g_input.hand[d].active) {
        rot = held_sword_rotation(sword_base(g_input.hand[d]), c);
        at = hand.pos;
    } else if (deku_leaf_item(game_item(link)) && ld32(link + kCurProc) == kProcLeafGlide) {
        // gliding on the Deku Leaf: the game does not draw it from his posed hand (it stayed above his head, where the
        // glide pose holds it, while the hands followed the controllers: "his hands are holding nothing", reported in
        // the headset). Placed from the hand it is measured on (measure_held: the nearest one gliding), as the game's
        // pose holds it there, so it stands up over your hands
        const Joint gh = read_joint(body, g_held.gameHand);
        if (!finite(gh.pos)) return;
        rot = normalize(gh.rot * g_held.itemRot);
        at = gh.pos + rotate(gh.rot, g_held.itemPos);
        // its handle in both hands: the leaf moved so that its joint nearest them (measure_held) is at the midpoint of
        // his two hands as posed
        const Vec3 h1 = read_joint(body, kArmJoints[1][2]).pos;
        const Vec3 mid = finite(h1) ? (gh.pos + h1) * 0.5f : gh.pos;
        if (g_held.leafHandleJoint >= 0 && finite(g_held.leafHandle)) at = mid - rotate(rot, g_held.leafHandle);
        // (diagnostics) once per glide: what is placed, from which hand, and how far from the eyes (the leaf in view?)
        static uint64_t loggedStep = 0;
        if (interp::logic_steps() > loggedStep + 30) {
            const Vec3 eyes = ld_vec(link + kPos) + g_link.eyeOffset + Vec3{0, g_link.eyeHeight, 0};
            LOG("[vr] Deku Leaf glide: item 0x%X model %08X placed from hand joint %d, its handle joint %d in the hand (%.0f "
                "units from it in the game's pose, %.0f from the leaf's root); the root %.0f from the eyes (%.0f above them)",
                game_item(link), model, g_held.gameHand, g_held.leafHandleJoint, double(g_held.leafHandleGap),
                double(length(g_held.leafHandle)), double(length(at - eyes)), double(at.y - eyes.y));
        }
        loggedStep = interp::logic_steps();
    } else if (itemHand == g_held.gameHand && g_bowHold.valid && bow_in_hand(game_item(link))) {
        // the bow in the hand the game holds it in: its place from the aiming pose (g_bowHold), not the game's
        // attachment of this action, which turned it flat or sideways while walking or standing
        rot = normalize(hand.rot * g_bowHold.rot);
        at = hand.pos + rotate(hand.rot, g_bowHold.pos);
        g_drawItemHand = itemHand;
    } else if (itemHand != g_held.gameHand) {
        // another item: the game's attachment, on its hand (the dominant one, or the off hand: off_hand_item),
        // mirrored onto it as the shield's is (his hands' frames are mirror images: unmirrored, an item moved to the
        // other hand is held upside down), flipped on its own axis across his body
        Quat r = g_held.itemRot;
        Vec3 p = g_held.itemPos;
        if (g_mirrorKnown) mirror_held(g_held.itemRot, g_held.itemPos, g_mirrorSign, g_held.itemAcross, r, p);
        rot = normalize(hand.rot * r);
        at = hand.pos + rotate(hand.rot, p);
        g_drawItemHand = itemHand;  // its hand closes around it (apply_hands)
        // The Wind Waker points forward, the way its controller points (asked for, 2026-11-10: "sometimes the windwaker
        // turn around and points at link instead of pointing forward ... can only point forward"). Its place in the hand
        // is the game's animation's, taken each step and mirrored onto this hand, and some of the conducting animations
        // turn it round in his hand. Its length axis (its own axis most along the controller's aim, chosen once while
        // held) pointing back against the aim: turned half round about its hand, across, so it points forward again
        if (game_item(link) == kWindWakerItem && g_input.valid) {
            const int side = aim_side(c, game_item(link));
            static int lengthAxis = -1;
            static uint32_t axisModel = 0;
            if (model != axisModel) lengthAxis = -1, axisModel = model;
            if (g_input.hand[side].active) {
                const Vec3 aim = normalize(to_game_dir(g_map, rotate(g_input.hand[side].aim.orientation, Vec3{0, 0, -1})));
                const Vec3 axes[3] = {rotate(rot, Vec3{1, 0, 0}), rotate(rot, Vec3{0, 1, 0}), rotate(rot, Vec3{0, 0, 1})};
                if (lengthAxis < 0 && finite(aim)) {
                    float best = -1;
                    for (int i = 0; i < 3; ++i)
                        if (std::fabs(dot(axes[i], aim)) > best) best = std::fabs(dot(axes[i], aim)), lengthAxis = i;
                    LOG("[vr] Wind Waker: its length is its axis %d (held pointing forward, along the controller)", lengthAxis);
                }
                if (lengthAxis >= 0 && finite(aim) && dot(axes[lengthAxis], aim) < 0) {
                    const Vec3 across = axes[(lengthAxis + 1) % 3];
                    const Quat half = axis_angle(across, kPi);
                    rot = normalize(half * rot);
                    at = hand.pos + rotate(half, at - hand.pos);
                    static uint64_t flipLogStep = 0;
                    static uint32_t flips = 0;
                    ++flips;
                    if (interp::logic_steps() > flipLogStep + 90) {
                        flipLogStep = interp::logic_steps();
                        LOG("[vr] Wind Waker: turned back to point forward (it pointed back at Link; %u frames so far)", flips);
                    }
                }
            }
        }
    } else {
        return;  // already in the dominant hand: the game's own attachment to the posed hand
    }
    // the sword moved by vr.swordOffset* in the sword-hand controller's grip space, so its handle sits in the fist
    // (right-handed play: the game's grip mirrored onto the hand; a picked-up weapon has vr.stickOffset*)
    if (g_held.sword && game_item(link) == kItemSword && !c.leftHanded && g_input.valid && g_input.hand[d].active)
        at = at + to_game_dir(g_map, rotate(g_input.hand[d].grip.orientation,
                                            Vec3{c.swordOffsetX, c.swordOffsetY, c.swordOffsetZ})) * world_scale();
    if (!finite(at)) return;
    ++g_heldPlaced;
    // the sword moved to Link's other hand: his hands are set for it after this (apply_hands)
    if (g_held.sword && swordHand != g_held.gameHand) g_drawSwordHand = swordHand;
    // the blade, and the hilt in the game's place on it: one sword
    g_heldTarget = {model, rot, at, true};
    // the Master Sword's glow (kSwordGlowModel): its base matrix moved as the blade is, before the game calculates it
    // later in this draw. It had stayed where the game holds the sword, in Link's left hand (reported in the headset,
    // right-handed). Moved from the blade's root as it is now, so a later pass of the same step moves it no further
    const uint32_t glow = g_held.sword ? ld32(link + kSwordGlowModel) : 0;
    uint16_t bladeCount = 0;
    const uint32_t bladeMtx = glow ? matrices_of(model, bladeCount) : 0;
    const Joint bladeBefore = bladeMtx ? read_joint(bladeMtx, 0) : Joint{};
    // a picked-up weapon is its own actor: placed in its own draw (after this one), from its own calculation
    if (game_item(link) != kItemEnemyWeapon) place(model, rot, at);
    if (bladeMtx && finite(bladeBefore.pos)) {
        move_matrices(glow + kModelBaseMtx, 1, bladeBefore.pos, at, normalize(rot * conjugate(bladeBefore.rot)));
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOG("[vr] sword: its glow moved with the blade");
        }
    }
    g_linkDrawnFrame = render::frame_count();
    if (g_held.sword && g_held.hiltOnHand) {
        g_held.hiltPlaced = at + rotate(rot, g_held.hiltPos);
        place(ld32(link + kHiltModel), normalize(rot * g_held.hiltRot), g_held.hiltPlaced);
    }
    // one piece with the hand holding it (each pass places it from that hand: frame_correct_pose)
    if (g_held.sword && game_item(link) != kItemEnemyWeapon) {
        lock_to_hand(kLockBlade, model, body, itemHand);
        if (g_held.hiltOnHand) lock_to_hand(kLockHilt, ld32(link + kHiltModel), body, itemHand);
    } else if (deku_leaf_item(game_item(link)) && ld32(link + kCurProc) == kProcLeafGlide) {
        // gliding: the open leaf one piece with its hand too (the blade's slot: no sword is in hand meanwhile). Placed
        // only at the step, it stayed while the hand was re-aimed each pass between steps, and came loose from the
        // hand and back (reported in the headset: "the leaf is moving in and out of Link's hands on its own")
        lock_to_both_hands(kLockBlade, model, body);
    }
}

// ---- the telescope (docs/VR_TELESCOPE.md)
// The game's telescope is its first-person camera (dCamera_c::subjectCamera) with zoom: while Link looks through it
// (player status 0 TELESCOPE_LOOK, play +0x5CD8 bit 0x200000) the camera's field of view narrows from the view's
// (by 0.444 at once, then the zoom the left stick's Y moves between its limits: dCamMath::zoomFovy). In VR the
// headset's field of view replaces the camera's, so that zoom never showed. Its magnification is the ratio of the
// tangents of the normal view's field of view (remembered from normal play) and the telescope's. The Picto Boxes
// (docs/VR_PICTO_BOX.md) aim with the same camera and zoom: the same magnification, from their own field of view, so
// each keeps its own limits.
// the Picto Box's viewfinder size while one is aimed in VR (vr.pictoBoxSize, 0.5 to 1), else 1: the layouts and the
// photo's frame (aspect.cpp picto_factors) take the same value
float picto_frame_scale() {
    if (!running() || telescope_view() || !picto_view()) return 1.0f;
    return std::clamp(config().pictoBoxSize, 0.5f, 1.0f);
}
std::atomic<float> g_normalFovy{0};
float telescope_magnification(float gameFovy) {
    if (!running() || !std::isfinite(gameFovy) || !(gameFovy > 1.0f && gameFovy < 170.0f)) return 1.0f;
    const bool scope = subject_zoom_view();
    if (!scope) {
        if (aspect::gameplay_camera()) g_normalFovy = gameFovy;
        return 1.0f;
    }
    const float normal = g_normalFovy.load() > 1.0f ? g_normalFovy.load() : 60.0f;
    const float m = zoom_magnification(normal, gameFovy);
    static float logged = 0;
    if (std::fabs(m - logged) > 0.25f * std::max(1.0f, logged)) {
        logged = m;
        LOG("[vr] %s: %.1fx (the game's field of view %.1f degrees, normally %.1f)", telescope_view() ? "telescope" : "Picto Box",
            double(m), double(gameFovy), double(normal));
    }
    return m;
}

// ---- pointing aim (docs/VR_AIMING.md)
// the body while aiming: setBodyAngleToCamera turns Link to the aim and bends his spine (mBodyAngle, joint 2 in
// jointBeforeCB) by its pitch. Pointing with a hand, that bent and twisted the body under the head (reported in the
// headset with the bow: the body out of place); the arrows, hooks and boomerang take their direction from
// mBodyAngle, so it stays, and only the spine's pose leaves it out
// (a quarter of the game's size, then a quarter of that: asked for twice in the headset)
// With the red laser on (vr.aimLaser, its dot at the sight's own point, bright on a valid target): not drawn at all,
// two dots showed with the hookshot otherwise (reported in the headset)
float sight_scale() {
    const bool on = running() && !g_thirdPerson.load() && aspect::gameplay_camera();
    const bool laser = on && config().aimLaser;
    static int logged = -1;
    if (on && logged != int(laser)) {
        logged = int(laser);
        LOG("[vr] aiming sight: %s", laser ? "not drawn (the laser's dot marks the point)" : "drawn at 1/16 of the game's size");
    }
    return !on ? 1.0f : laser ? 0.0f : 0.0625f;
}
// ---- the hands every frame (docs/VR_ARM_ANIMATION.md, "Hands every frame")
// The arms are aimed once a step (link_arms) and in-between frames blend two steps' arms, so the drawn hands trailed
// the controllers by about a step, and by a different amount each frame as frames came unevenly: shaking hands while
// moving, turning and stick turning (reported in the headset). Each pass the drawn (blended) arm is re-solved from its
// own shoulder, bone lengths and elbow plane to the wrist moved by how far the controller's target has moved since
// (g_frameHand, from the camera override), and the hand turned by the controller's turn since; his hands model and the
// models he holds move rigidly with the hand. The step's styling (the elbow, the shoulders, the sword's wrist) stays.
constexpr uint32_t kHandsModel = 0xCF4;  // daPy_lk_c::mpHandsModel
constexpr uint32_t kBraceletModel = 0x4434;  // daPy_lk_c::mpPringModel (the Power Bracelets)
float be_float(uint32_t w) {
    w = __builtin_bswap32(w);
    float f;
    std::memcpy(&f, &w, 4);
    return f;
}
uint32_t float_be(float f) {
    uint32_t w;
    std::memcpy(&w, &f, 4);
    return __builtin_bswap32(w);
}
Joint be_joint(const uint32_t* m) {
    float f[12];
    for (int i = 0; i < 12; ++i) f[i] = be_float(m[i]);
    return joint_from_matrix(f);
}
void put_be_joint(uint32_t* m, const Joint& j) {
    float f[12];
    matrix_from_joint(j, f);
    for (int i = 0; i < 12; ++i) m[i] = float_be(f[i]);
}
// a joint moved rigidly with a hand's move
Joint moved_with(const Joint& j, const HandMove& h) {
    Joint out = j;
    out.rot = normalize(h.rot * j.rot);
    out.pos = h.to + rotate(h.rot, j.pos - h.from);
    return out;
}
static bool frame_correct_pose(uint32_t model, uint32_t n, uint32_t* w, float t) {
    (void)t;
    if (!model || !w || !running() || g_thirdPerson.load() || !play_view()) return false;
    const uint32_t link = g_link.link;
    if (!link || !g_link.valid) return false;
    const bool body = model == ld32(link + kBodyModel);
    const bool hands = model == ld32(link + kHandsModel);
    const bool heldModel = model == ld32(link + kEquipModel) || model == ld32(link + kHiltModel) ||
                           model == ld32(link + kShieldModel) || model == ld32(link + kSwordGlowModel);
    // the Power Bracelets: the game copies his forearms into its joints 1 (left) and 2 (right) each step
    // (daPy_lk_c's setItemModel: CL_JNT_LARMB / RARMB); they shook on the re-aimed arms (reported in the headset)
    const bool bracelets = model == ld32(link + kBraceletModel);
    // a weapon picked up from an enemy (the stick): its own actor's model, placed at the tracked hand by its draw
    // (weapon_drawing) once a step's hand; it shook back and forth against the hand while the view turned (the right
    // stick: the hands re-aimed every frame, the stick not; reported in the headset, as the bracelets had)
    const uint32_t keptWeapon = game_item(link) == kItemEnemyWeapon ? ld32(link + kKeptEquipActor) : 0;
    const bool weapon = keptWeapon && model == ld32(keptWeapon + kBokoModel);
    // the arrow on the bow (g_bowLine): the bow aimed in first person, the arrow kept on Link's hand
    const uint32_t keptArrow = keep_bow_hand(link) && bow_in_hand(ld16(link + 0x69B0)) ? ld32(link + kKeptEquipActor) : 0;
    const bool arrow = keptArrow && model == ld32(keptArrow + kArrowModel);
    if (!body && !hands && !heldModel && !bracelets && !weapon && !arrow) return false;
    const Config c = config();
    if (!c.armIK) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_head.calibrated()) return false;
    const uint64_t pass = g_passSerial;
    if (body) {
        bool changed = false;
        // the view's turn by the stick since the step drawn (g_head.turn_total() against the steps' own, blended as Link's model
        // is): his whole body turned by it, about where he stands, every pass. His facing follows a turn only at the
        // next step, and his model between two steps' facings: while the stick turned the view his body trailed it
        // and the arms, on the controllers every pass, twisted against it (reported in the headset). Turns only: not
        // recentring, ladders or ledges (the view turns to Link there)
        const float stepTurn = g_link.turnPrev + (g_link.turnCur - g_link.turnPrev) * std::clamp(t, 0.0f, 1.0f);
        const float bodyTurn = g_head.turn_total() - stepTurn;
        const bool turning = std::isfinite(bodyTurn) && std::fabs(bodyTurn) > 1e-5f && std::fabs(bodyTurn) < 3.0f;
        const Quat turnRot = yaw_quat(turning ? bodyTurn : 0.0f);
        // (about his eye point, blended as the anchor is: where the view turns about; vr_camera.h turn_pivot)
        const float tc = std::clamp(t, 0.0f, 1.0f);
        const Vec3 eyeOffset = g_link.eyeBlendSeeded ? lerp(g_link.prevEyeOffset, g_link.eyeOffset, tc) : g_link.eyeOffset;
        const Vec3 pivot = turn_pivot(lerp(g_link.prevPos, g_link.pos, tc), eyeOffset, g_link.offsetSeeded);
        g_bodyTurn = {turning, pivot, turnRot, pass};
        Joint handBlend[2], lowerBlend[2];  // as blended, before the turn (what models on them are placed against)
        for (int set = 0; set < 2; ++set) {
            const uint32_t jl = uint32_t(kArmJoints[set][1]), jh = uint32_t(kArmJoints[set][2]);
            if (jh < n) handBlend[set] = be_joint(w + 12 * jh);
            if (jl < n) lowerBlend[set] = be_joint(w + 12 * jl);
        }
        if (turning) {
            for (uint32_t j = 0; j < n; ++j) {
                Joint jt = be_joint(w + 12 * j);
                if (!finite(jt.pos)) continue;
                jt.pos = pivot + rotate(turnRot, jt.pos - pivot);
                jt.rot = normalize(turnRot * jt.rot);
                put_be_joint(w + 12 * j, jt);
            }
            changed = true;
        }
        for (int set = 0; set < 2; ++set) {  // each hand as drawn (corrected below where its controller drives it)
            const uint32_t jl = uint32_t(kArmJoints[set][1]), jh = uint32_t(kArmJoints[set][2]);
            g_handMove[set].valid = g_foreMove[set].valid = false;
            if (jh >= n) continue;
            const Joint h = be_joint(w + 12 * jh);
            if (g_drawnHand[set].serial != pass) g_drawnHandPrev[set] = g_drawnHand[set];  // (the pass before: lock_target)
            if (finite(h.pos)) g_drawnHand[set] = {h.rot, h.pos, pass};
            if (turning && finite(handBlend[set].pos) && finite(lowerBlend[set].pos)) {
                // (what is on the hand and the forearm turns with them: a controller below may move them further)
                g_handMove[set] = {true, handBlend[set].pos, h.pos, normalize(h.rot * conjugate(handBlend[set].rot)), pass};
                const Joint lo = be_joint(w + 12 * jl);
                g_foreMove[set] = {true, lowerBlend[set].pos, lo.pos, normalize(lo.rot * conjugate(lowerBlend[set].rot)), pass};
            }
        }
        for (int side = 0; side < 2; ++side) {  // (controller side: its Link arm's joints, its tracking weight)
            const int set = g_jointSet[side];
            HandMove& move = g_handMove[set];
            const FrameHand& f = g_frameHand[side];
            const float weight = g_armWeight[side];
            if (!f.valid || f.serial != pass || weight <= 0) continue;
            const uint32_t ju = uint32_t(kArmJoints[set][0]), jl = uint32_t(kArmJoints[set][1]), jh = uint32_t(kArmJoints[set][2]);
            if (jh >= n) continue;
            Joint upper = be_joint(w + 12 * ju), lower = be_joint(w + 12 * jl), hand = be_joint(w + 12 * jh);
            if (!finite(upper.pos) || !finite(lower.pos) || !finite(hand.pos)) continue;
            const ArmChain chain{length(lower.pos - upper.pos), length(hand.pos - lower.pos)};
            if (!(chain.upper > 1.0f && chain.lower > 1.0f)) continue;
            // the elbow's side of the shoulder-to-wrist line, as drawn: the same bend plane
            const Vec3 line = normalize(hand.pos - upper.pos);
            Vec3 pole = (lower.pos - upper.pos) - line * dot(lower.pos - upper.pos, line);
            if (!finite(pole) || length(pole) < 1e-3f) pole = Vec3{0, -1, 0};
            // the controller's place this pass (its offset is against the blended hand: the step's place in the
            // view of now); with the arm's weight below 1, between that and the hand as turned with the body
            const Vec3 target = lerp(hand.pos, handBlend[set].pos + f.delta, weight);
            const ArmSolution sol = solve_arm(upper.pos, target, pole, chain);
            if (!finite(sol.elbow) || !finite(sol.wrist)) continue;
            upper.rot = normalize(rotation_between(lower.pos - upper.pos, sol.elbow - upper.pos) * upper.rot);
            lower.rot = normalize(rotation_between(hand.pos - lower.pos, sol.wrist - sol.elbow) * lower.rot);
            lower.pos = sol.elbow;
            hand.pos = sol.wrist;
            hand.rot = normalize(slerp(normalize(turnRot * handBlend[set].rot),
                                       f.haveHandNow ? f.handNow : normalize(f.turn * handBlend[set].rot), weight));
            // the shield hand on the shield's handle this pass, as at the step (shield_hand)
            if (side != dominant_side(c)) {
                Joint at = hand;
                at.pos = sol.wrist;
                Quat onHandle;
                if (shield_hand(at, c, dominant_side(c), onHandle)) hand.rot = normalize(slerp(hand.rot, onHandle, weight));
            }
            put_be_joint(w + 12 * ju, upper);
            put_be_joint(w + 12 * jl, lower);
            put_be_joint(w + 12 * jh, hand);
            g_drawnHand[set] = {hand.rot, hand.pos, pass};
            move.from = handBlend[set].pos;
            move.to = hand.pos;
            move.rot = normalize(hand.rot * conjugate(handBlend[set].rot));
            move.valid = true;
            move.serial = pass;
            HandMove& fore = g_foreMove[set];
            fore.from = lowerBlend[set].pos;
            fore.to = lower.pos;
            fore.rot = normalize(lower.rot * conjugate(lowerBlend[set].rot));
            fore.valid = true;
            fore.serial = pass;
            changed = true;
        }
        static uint64_t logged = 0;
        if (changed && ++logged % 900 == 1) {
            const FrameHand& f = g_frameHand[dominant_side(c)];
            LOG("[vr] hands every frame: arms re-aimed to the controllers (the sword hand's moved %.1f units this frame)",
                f.valid ? double(length(f.delta)) : 0.0);
        }
        return changed;
    }
    // his hands model (each joint with its hand) and what he holds (the whole model with the hand it is on)
    auto nearest = [&](Vec3 at) -> const HandMove* {
        const HandMove* best = nullptr;
        float bestD = 25.0f;
        for (const HandMove& h : g_handMove)
            if (h.valid && h.serial == pass) {
                const float d = length(at - h.from);
                if (d < bestD) bestD = d, best = &h;
            }
        return best;
    };
    bool changed = false;
    if (bracelets) {
        for (int set = 0; set < 2; ++set) {
            const uint32_t j = uint32_t(set + 1);
            const HandMove& f = g_foreMove[set];
            if (j >= n || !f.valid || f.serial != pass) continue;
            put_be_joint(w + 12 * j, moved_with(be_joint(w + 12 * j), f));
            changed = true;
        }
        return changed;
    }
    if (weapon) {
        // rigidly in the hand as drawn this pass (g_drawnHand, g_weaponGrip): its base at that hand times its grip, its
        // joints moved there from the base their (blended) root is in. The hand's place of the pass before when the
        // weapon is drawn before the body in a pass
        const int set = g_jointSet[dominant_side(c)];
        const DrawnHand& hd = g_drawnHand[set];
        const bool fresh = hd.serial == pass, stale = hd.serial + 1 == pass;
        if (!g_weaponGrip.valid || !g_weaponGrip.rootValid || g_weaponGrip.set != set || !(fresh || stale)) {
            ++g_weaponPlaced.none;
            return false;
        }
        const Quat baseRot = normalize(hd.rot * g_weaponGrip.rot);
        const Vec3 basePos = hd.pos + rotate(hd.rot, g_weaponGrip.pos);
        const Joint root = be_joint(w);
        const Quat fromRot = normalize(root.rot * conjugate(g_weaponGrip.rootRot));
        const Vec3 fromPos = root.pos - rotate(fromRot, g_weaponGrip.rootPos);
        if (!finite(basePos) || !finite(fromPos)) { ++g_weaponPlaced.none; return false; }
        HandMove m;
        m.valid = true;
        m.from = fromPos;
        m.to = basePos;
        m.rot = normalize(baseRot * conjugate(fromRot));
        for (uint32_t j = 0; j < n; ++j) put_be_joint(w + 12 * j, moved_with(be_joint(w + 12 * j), m));
        ++(fresh ? g_weaponPlaced.fresh : g_weaponPlaced.stale);
        return true;
    }
    if (arrow) {
        // along the line from the drawing hand (the left, vr_bow.h) through the bow hand, as drawn this pass (or the
        // pass before, drawn before his body); its model's +Z is the way it flies, its origin the nock (a stuck arrow's
        // model is placed 50 units back from where it hit: setStopActorMatrix; placed at the head, the whole arrow stood
        // an arrow's length ahead of the hand, reported in the headset)
        const DrawnHand& hs = g_drawnHand[g_jointSet[0]];
        const DrawnHand& hb = g_drawnHand[g_jointSet[1]];
        auto usable = [&](const DrawnHand& h) { return h.serial == pass || h.serial + 1 == pass; };
        if (!usable(hs) || !usable(hb) || !finite(hs.pos) || !finite(hb.pos)) return false;
        const Vec3 nock = hs.pos, toBow = hb.pos - nock;
        if (!(length(toBow) > 1.0f)) return false;
        const Vec3 dir = normalize(toBow);
        Vec3 head = nock + dir * kArrowLength;
        const float past = dot(head - hb.pos, dir);
        if (past < kHeadPastBow) head = head + dir * (kHeadPastBow - past);
        // its roll from the bow hand's up (the fletching turns with the bow)
        Vec3 side = cross(rotate(hb.rot, Vec3{0, 1, 0}), dir);
        if (!(length(side) > 1e-3f)) side = cross(Vec3{0, 1, 0}, dir);
        if (!(length(side) > 1e-3f)) side = Vec3{1, 0, 0};
        side = normalize(side);
        const Quat rot = from_basis(side, cross(dir, side), dir);
        const Joint root = be_joint(w);
        if (!finite(root.pos) || !finite(head)) return false;
        HandMove m;
        m.valid = true;
        m.from = root.pos;
        m.to = head - dir * kArrowLength;  // (the nock: at the drawing hand, or behind it with the head held past the bow)
        m.rot = normalize(rot * conjugate(root.rot));
        for (uint32_t j = 0; j < n; ++j) put_be_joint(w + 12 * j, moved_with(be_joint(w + 12 * j), m));
        {
            std::lock_guard<std::mutex> lock(g_bowLineMutex);
            g_bowLine = {true, nock, dir, head, keptArrow};
        }
        return true;
    }
    // the sword and the shield: one piece with the hand holding them, placed from it as drawn this pass, their root
    // where it was in that hand at the step (g_locks). Drawn before his body in a pass, the hand is the last pass's
    // carried on by its own last move: as it was, while Link moved the sword trailed the hand by a pass, by a different
    // amount as the order changed, and flickered about it (reported in the headset)
    auto handNow = [&](int set, Quat& rot, Vec3& pos) {
        const DrawnHand& hd = g_drawnHand[set];
        if (!finite(hd.pos)) return 0;
        rot = hd.rot;
        pos = hd.pos;
        if (hd.serial == pass) return 1;
        if (hd.serial + 1 != pass) return 0;
        const DrawnHand& before = g_drawnHandPrev[set];
        if (before.serial + 1 == hd.serial && finite(before.pos) && length(hd.pos - before.pos) < 30.0f)
            pos = hd.pos + (hd.pos - before.pos);
        return 2;
    };
    auto lockTarget = [&](const HandLock& l, Quat& rot, Vec3& to) {
        Quat hr;
        Vec3 hp;
        const int how = handNow(l.set, hr, hp);
        if (!how) return 0;
        if (l.both) {  // (the leaf's glide: from both hands' midpoint, turned with the left hand)
            Quat hr1;
            Vec3 hp1;
            const int how1 = handNow(1, hr1, hp1);
            if (!how1) return 0;
            rot = normalize(hr * l.rot);
            to = (hp + hp1) * 0.5f + rotate(hr, l.pos);
            return std::max(how, how1);
        }
        rot = normalize(hr * l.rot);
        to = hp + rotate(hr, l.pos);
        // the shield: by the same rule as at the step (its controller, levelled), at the hand as drawn; from the hand's
        // turn instead, it rolled with the wrist between steps and was levelled again at each, shaking
        if (&l == &g_locks[kLockShield]) {
            Joint at;
            at.rot = hr;
            at.pos = hp;
            Quat r;
            Vec3 p;
            if (shield_by_controller(at, c, dominant_side(c), r, p)) rot = r, to = p;
        }
        return how;
    };
    for (const HandLock& l : g_locks) {
        if (!l.valid || l.model != model) continue;
        const Joint root = be_joint(w);
        Quat rot;
        Vec3 to;
        const int how = finite(root.pos) ? lockTarget(l, rot, to) : 0;
        if (!how) {
            ++g_lockPlaced.none;
            break;
        }
        ++(how == 1 ? g_lockPlaced.fresh : g_lockPlaced.carried);
        HandMove m;
        m.valid = true;
        m.from = root.pos;
        m.to = to;
        m.rot = normalize(rot * conjugate(root.rot));
        for (uint32_t j = 0; j < n; ++j) put_be_joint(w + 12 * j, moved_with(be_joint(w + 12 * j), m));
        return true;
    }
    // the Master Sword's glow: its root on the blade's root as the blade is placed this pass (its base is the blade's
    // root at the step: link_draw_begin). Moved from where the drawn glow is this pass, which between steps is the
    // blend of two steps' places, not the step's: from the step's place it landed off the blade by Link's move since,
    // flickering while he ran (reported in the headset: "sync sword glow to the sword")
    if (model == ld32(link + kSwordGlowModel) && g_locks[kLockBlade].valid) {
        Quat rot;
        Vec3 to;
        const Joint root = be_joint(w);
        if (finite(root.pos) && lockTarget(g_locks[kLockBlade], rot, to)) {
            HandMove m;
            m.valid = true;
            m.from = root.pos;
            m.to = to;
            m.rot = normalize(rot * conjugate(root.rot));
            for (uint32_t j = 0; j < n; ++j) put_be_joint(w + 12 * j, moved_with(be_joint(w + 12 * j), m));
            return true;
        }
    }
    // what is on no hand turns with his body (g_bodyTurn: the stick's turn since the step)
    const bool bodyTurning = g_bodyTurn.valid && g_bodyTurn.serial == pass;
    auto turned = [&](Joint jt) {
        jt.pos = g_bodyTurn.pivot + rotate(g_bodyTurn.rot, jt.pos - g_bodyTurn.pivot);
        jt.rot = normalize(g_bodyTurn.rot * jt.rot);
        return jt;
    };
    if (hands) {
        for (uint32_t j = 0; j < n; ++j) {
            Joint jt = be_joint(w + 12 * j);
            if (const HandMove* h = finite(jt.pos) ? nearest(jt.pos) : nullptr) {
                put_be_joint(w + 12 * j, moved_with(jt, *h));
                changed = true;
            } else if (bodyTurning && finite(jt.pos)) {
                put_be_joint(w + 12 * j, turned(jt));
                changed = true;
            }
        }
        return changed;
    }
    const Joint root = be_joint(w);
    const HandMove* h = finite(root.pos) ? nearest(root.pos) : nullptr;
    if (!h && bodyTurning && finite(root.pos)) {  // (on his back, say: the sheathed sword, the shield)
        for (uint32_t j = 0; j < n; ++j) put_be_joint(w + 12 * j, turned(be_joint(w + 12 * j)));
        return true;
    }
    if (!h) return false;
    for (uint32_t j = 0; j < n; ++j) put_be_joint(w + 12 * j, moved_with(be_joint(w + 12 * j), *h));
    // the bow's string at the drawing hand while the arrow is on (g_bowString): the string bends back with the hand
    if (g_bowOnBow && g_bowString.model == model && g_bowString.joint > 0 && uint32_t(g_bowString.joint) < n) {
        const DrawnHand& sh = g_drawnHand[g_jointSet[0]];  // (the arrow's hand: the left, vr_bow.h)
        if ((sh.serial == pass || sh.serial + 1 == pass) && finite(sh.pos)) {
            for (const int sjIndex : {g_bowString.joint, g_bowString.joint2}) {  // (the top and bottom halves)
                if (sjIndex <= 0 || uint32_t(sjIndex) >= n) continue;
                Joint sj = be_joint(w + 12 * uint32_t(sjIndex));
                sj.pos = sh.pos;
                put_be_joint(w + 12 * uint32_t(sjIndex), sj);
            }
        }
    }
    return true;
}
// Link's body hidden in first person (vr.hideBody, docs/VR_FIRST_PERSON_CAMERA.md "Hiding the body"): only his arms,
// his hands and what they hold are drawn. Each pass, after the arms were posed and the hands' moves taken from them
// (frame_correct_pose: his hands model and what he holds are placed from those), the body's drawn joint matrices except
// the arms' (5 to 12: the clavicles, upper arms, forearms and the body's own hands, an empty hand being a mesh of his
// body) map everything onto his neck's base, the point the head is already collapsed onto: his torso, legs, head and
// clothes draw nothing, and the skin shared between his shoulders and chest shrinks towards the neck rather than
// stretching across. The shield and sword while they are on his back go too. Only these drawn matrices change, never
// his animation's (his next pass draws him from those again): third person, events and the setting switched off draw
// him as before at once. Switching shapes off instead did not hide him: his draw enters his model with the visibility it
// sets itself
constexpr uint32_t kFirstArmJoint = 5, kLastArmJoint = 12;
static bool hides_from_view(uint32_t link, uint32_t model) {
    if (!config().hideBody || g_thirdPerson.load() || !aspect::gameplay_camera() || subject_zoom_view()) return false;
    if (model == ld32(link + kBodyModel)) return true;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (model == ld32(link + kShieldModel)) return !g_held.shieldOnHand;
    if (model == ld32(link + kEquipModel)) return g_held.gameHand < 0;  // (sheathed, stowed: on his back)
    // the sword's hilt and glow only with the sword in his hand: holding another item (the Deku Leaf) the sheathed
    // sword's hilt on his back was drawn, and flickered in and out of view as the leaf's models swapped at each use
    // (reported in the headset: "looks like the sword is popping into the view")
    if (model == ld32(link + kHiltModel)) return !(g_held.sword && g_held.gameHand >= 0 && g_held.hiltOnHand);
    if (model == ld32(link + kSwordGlowModel)) return !(g_held.sword && g_held.gameHand >= 0);
    return false;
}
// Link's arms hidden in first person (vr.hideArms, docs/VR_FIRST_PERSON_CAMERA.md "Hiding the arms"): each arm's
// clavicle, upper arm and forearm drawn onto its hand's place, so the arm draws nothing and the hand (and what it
// holds, placed from it before this) stays where it is. Only the drawn matrices, as for the body
static bool hide_arms(uint32_t link, uint32_t model, uint32_t n, uint32_t* w) {
    if (!config().hideArms || g_thirdPerson.load() || !aspect::gameplay_camera() || subject_zoom_view()) return false;
    if (model != ld32(link + kBodyModel)) return false;
    // with his body hidden too (hides_from_view), the arms go onto the neck's base with it: onto the hands, the skin
    // the game weights between a shoulder and his chest stretched from each hand to the neck, a thin green thread from
    // the hands (reported in the headset). His hands are shapes of their own, held by the hand joints alone, so
    // nothing stretches from them
    const bool withBody = hides_from_view(link, model) && n > uint32_t(kNeckJoint);
    bool done = false;
    for (int side = 0; side < 2; ++side) {
        const int set = g_jointSet[side];
        const uint32_t hand = uint32_t(kArmJoints[set][2]);
        if (hand >= n) continue;
        const uint32_t* at = w + 12 * (withBody ? uint32_t(kNeckJoint) : hand);
        const uint32_t x = at[3], y = at[7], z = at[11];
        for (int j : {kClavicleJoints[set], kArmJoints[set][0], kArmJoints[set][1]}) {
            if (j < 0 || uint32_t(j) >= n) continue;
            uint32_t* m = w + 12 * uint32_t(j);
            for (int i = 0; i < 12; ++i) m[i] = 0;
            m[3] = x, m[7] = y, m[11] = z;
        }
        done = true;
    }
    static uint32_t logged = 0;
    if (done && logged != link) {
        logged = link;
        LOG("[vr] first person: Link's arms hidden (vr.hideArms; onto %s); his hands drawn",
            withBody ? "his neck with his hidden body" : "his hands");
    }
    return done;
}
bool frame_correct(uint32_t model, uint32_t n, uint32_t* w, float t) {
    bool changed = frame_correct_pose(model, n, w, t);
    if (!model || !w || !n || !running() || !play_view()) return changed;
    const uint32_t link = g_link.link;
    if (!link || !g_link.valid) return changed;
    // (the spin attack's view turn: his shoulders as drawn, before hide_arms and the body's hiding fold them away)
    if (model == ld32(link + kBodyModel) && n > uint32_t(kClavicleJoints[1])) {
        const Vec3 across = be_joint(w + 12 * kClavicleJoints[1]).pos - be_joint(w + 12 * kClavicleJoints[0]).pos;
        g_drawnShoulders.store(finite(across) && across.x * across.x + across.z * across.z > 1.0f
                                   ? std::atan2(across.x, across.z) : NAN, std::memory_order_relaxed);
        // (diagnostics, the spin's view turn: what the drawn body gives, every 2 s)
        static auto last = std::chrono::steady_clock::time_point{};
        if (const auto now = std::chrono::steady_clock::now(); now - last > std::chrono::seconds(2)) {
            last = now;
            const Joint j0 = be_joint(w), j1 = be_joint(w + 12);
            LOG("[vr] spin diag: drawn body %u joints, shoulders %.1f apart (%.1f %.1f %.1f), yaw %.0f; joint 0 at %.1f %.1f "
                "%.1f yaw %.0f, joint 1 yaw %.0f", n, double(length(across)), double(across.x), double(across.y),
                double(across.z), double(std::atan2(across.x, across.z) * 180.0f / kPi), double(j0.pos.x),
                double(j0.pos.y), double(j0.pos.z), double(yaw_of(j0.rot) * 180.0f / kPi), double(yaw_of(j1.rot) * 180.0f / kPi));
        }
    } else if (model == ld32(link + kBodyModel)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOG("[vr] spin diag: drawn body has only %u joints (shoulders are %d and %d)", n, kClavicleJoints[0],
                kClavicleJoints[1]);
        }
    }
    changed |= hide_arms(link, model, n, w);  // (before the body: it reads the hands' places, the body keeps the arms)
    if (!hides_from_view(link, model)) return changed;
    // the body: all but the arms onto the neck's base (its drawn place: hide_head collapsed it onto itself); what is on his
    // back: everything onto its root's place
    const bool body = model == ld32(link + kBodyModel);
    if (body && n <= uint32_t(kNeckJoint)) return changed;
    const uint32_t* at = w + 12 * (body ? uint32_t(kNeckJoint) : 0u);
    const uint32_t x = at[3], y = at[7], z = at[11];
    for (uint32_t j = 0; j < n; ++j) {
        if (body && j >= kFirstArmJoint && j <= kLastArmJoint) continue;
        uint32_t* m = w + 12 * j;
        for (int i = 0; i < 12; ++i) m[i] = 0;
        m[3] = x, m[7] = y, m[11] = z;
    }
    static uint32_t logged = 0;
    if (logged != link) {
        logged = link;
        LOG("[vr] first person: Link's body hidden (vr.hideBody); his arms, hands and what he holds drawn");
    }
    return true;
}
// end of input::read() (mods/camera.cpp): the controllers as the GamePad, then the buttons a text prompt (the new save's
// name, overlay/text_entry.h) left held kept from the game until released: the A that pressed OK would otherwise reach
// the game as a new press the moment the prompt closed (the overlay's own release gate covers the desktop's controllers,
// not these)
void filter_pad(input::PadState& s) {
    filter_pad_controllers(s);
    static uint32_t held = 0;
    if (!running()) {  // (the desktop: the overlay's own gate, as before)
        held = 0;
        return;
    }
    if (text_entry::active()) held = s.buttons;  // (the game gets nothing meanwhile: overlay::blocks_input)
    else held &= s.buttons;                      // released ones go
    s.buttons &= ~held;
}
bool menu_open() { return running() && g_menuOpen.load(); }

// The new save's name (docs/VR_MENU.md, "The name of a new save"): the game asks the system keyboard for it (swkbd) on
// the title or file select, and the settings overlay's text prompt stands in for the GamePad's keyboard
// (overlay/text_entry.h: the name field, an on-screen keyboard in the game's name font, its OK key). In VR it showed on
// the desktop only and took the desktop's controllers: the headset showed the GamePad picture, and the VR controllers
// reached nothing (the game gets no input while the prompt shows). Now, while it asks there, the prompt shows on the
// headset's panel (vr_vulkan.cpp) and the controllers drive its keyboard: the left stick moves between keys, A types
// the key chosen (the OK key ends it), B deletes the last character, X space, Y shift, the grips change pages.
// (2026-11-07) Every prompt, not only the title's and file select's: in play (the settings page's rename, upstream
// v0.5.1, and the game's own keyboards) it showed on the desktop alone and the headset kept the game, frozen for input
bool name_entry_open() { return running() && text_entry::active(); }
namespace {
bool g_namePad = false;  // the controllers drive the name's keyboard
}
static void menu_pad(const xr::InputState& in, bool leftHanded, float* v);
// (each pass, game thread) the controllers to the name's keyboard while it shows; off again when it closes
static void name_entry_pass() {
    const bool on = name_entry_open() && !g_menuOpen.load();
    if (!on) {
        if (g_namePad) {
            g_namePad = false;
            overlay::set_vr_controller(nullptr);
            text_entry::set_vr(false);
            LOG("[vr] name entry: the keyboard closed; the controllers are the game's again");
        }
        return;
    }
    const Config c = config();
    xr::InputState in;
    if (!session().sync_input(display_time(), c.leftHanded, in)) return;
    float pad[input_map::kPadCount];
    menu_pad(in, c.leftHanded, pad);
    overlay::set_vr_controller(pad);
    if (!g_namePad) {
        g_namePad = true;
        text_entry::set_vr(true);
        LOG("[vr] name entry: the keyboard shows in the headset (left stick: keys, A: type, B: delete, OK: done)");
    }
}

// The headset's controllers as a gamepad for the menu, by their physical places (either handedness): the left stick
// and the right stick move between options; A confirms and B goes back (the right controller's buttons, as the game's
// A and B), X and Y as the game's; the grips change tabs (L / R); the triggers are ZL / ZR
static void menu_pad(const xr::InputState& in, bool leftHanded, float* v) {
    using namespace input_map;
    std::fill(v, v + kPadCount, 0.0f);
    auto bit = [&](Action a) { return (in.held & (1u << int(a))) != 0 ? 1.0f : 0.0f; };
    const Action rA = leftHanded ? Action::OffPrimary : Action::MainPrimary, rB = leftHanded ? Action::OffSecondary : Action::MainSecondary;
    const Action lX = leftHanded ? Action::MainPrimary : Action::OffPrimary, lY = leftHanded ? Action::MainSecondary : Action::OffSecondary;
    const Action lGrip = leftHanded ? Action::MainGrip : Action::OffGrip, rGrip = leftHanded ? Action::OffGrip : Action::MainGrip;
    const Action lTrig = leftHanded ? Action::MainTrigger : Action::OffTrigger, rTrig = leftHanded ? Action::OffTrigger : Action::MainTrigger;
    v[face_input(kA)] = bit(rA);
    v[face_input(kB)] = bit(rB);
    v[face_input(kX)] = bit(lX);
    v[face_input(kY)] = bit(lY);
    v[kPadLB] = bit(lGrip);
    v[kPadRB] = bit(rGrip);
    v[kPadLT] = bit(lTrig);
    v[kPadRT] = bit(rTrig);
    const float lx = in.hand[0].stickX, ly = in.hand[0].stickY, rx = in.hand[1].stickX, ry = in.hand[1].stickY;
    v[kPadLSUp] = std::max(0.0f, ly);
    v[kPadLSDown] = std::max(0.0f, -ly);
    v[kPadLSLeft] = std::max(0.0f, -lx);
    v[kPadLSRight] = std::max(0.0f, lx);
    // the right stick as the D-pad (one option a push)
    v[kPadDUp] = ry > 0.6f ? 1.0f : 0.0f;
    v[kPadDDown] = ry < -0.6f ? 1.0f : 0.0f;
    v[kPadDLeft] = rx < -0.6f ? 1.0f : 0.0f;
    v[kPadDRight] = rx > 0.6f ? 1.0f : 0.0f;
}

// Each pass, before the game's (interp.cpp, game thread): while the VR menu is open the pass only draws (the game
// paused: no logic, timers or input), the controllers drive the menu, and the left stick's click closes it. The
// menu closed another way (B at its top, its close button, Esc) ends the pause too.
bool menu_pause_pass() {
    // the headset taken off (vr.h headset_away): the game held, its picture kept as it was, until it is back
    if (vr::headset_away()) return true;
    name_entry_pass();
    if (g_menuRequest.exchange(false) && running() && !g_menuOpen.load()) {
        g_menuOpen = true;
        overlay::open_vr_settings();
        LOG("[vr] menu: opened by the left stick's click (the game paused)");
    }
    if (!g_menuOpen.load()) return false;
    if (!running() || !overlay::is_open()) {
        g_menuOpen = false;
        overlay::set_vr_controller(nullptr);
        LOG("[vr] menu: closed (the game runs again)");
        return false;
    }
    const Config c = config();
    xr::InputState in;
    if (!session().sync_input(display_time(), c.leftHanded, in)) return true;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        // the hands keep following the controllers (their poses); nothing of the game's input is used
        for (int h = 0; h < 2; ++h) g_input.hand[h] = in.hand[h];
        g_input.valid = in.valid;
    }
    float pad[input_map::kPadCount];
    menu_pad(in, c.leftHanded, pad);
    overlay::set_vr_controller(pad);
    const bool down = (in.held & (1u << int(c.leftHanded ? Action::MainStickClick : Action::OffStickClick))) != 0;
    const bool close = down && !g_menuClickHeld;
    g_menuClickHeld = down;
    if (close) {
        overlay::set_vr_controller(nullptr);
        overlay::set_open(false);
        g_menuOpen = false;
        LOG("[vr] menu: closed by the left stick's click (the game runs again)");
        return false;
    }
    return true;
}

bool bomb_in_hand() {
    const uint32_t link = g_link.link;
    return running() && link && g_link.valid && !g_thirdPerson.load() && first_person_play() && carried_bomb(link) != 0;
}
void arrow_kept(uint32_t arrow) {
    // the arrow drawn along the line (g_bowLine.arrow), also after the shot: Link's bow code marks it shot and lets go
    // of it (checkNextActionBowReady: mActorKeepEquip cleared) before its own step shoots it here (procWait); checked
    // as still kept, this never reached the shot, and it flew along the game's angles (reported in the headset)
    const uint32_t link = g_link.link;
    if (!arrow || !link || !g_link.valid || !running() || g_thirdPerson.load()) return;
    BowLine line;
    {
        std::lock_guard<std::mutex> lock(g_bowLineMutex);
        line = g_bowLine;
    }
    if (!line.valid || line.arrow != arrow || !finite(line.dir) || !finite(line.head)) return;
    // its angles as the game's own sets them from its matrix: shape_angle from the direction (x down positive, y the
    // heading), current.angle x = -shape_angle.x, y = shape_angle.y (arrowShooting flies along current.angle). Its place
    // stays the game's: moved to the nock, the shot's first collision line (from where it was) ran into the ground and
    // the arrows stuck there (reported in the headset)
    // let go before drawn back: it falls out of the hand (asked for in the headset: not shot to the ground). The game
    // shoots it along the bow as any arrow; from the next step arrow_fall_step sets its speed: a little forward, and
    // falling from rest
    bool drop = false;
    {
        std::lock_guard<std::mutex> lock(g_bowLineMutex);
        drop = g_bowDropArrow == arrow && ld32(link + kKeptEquipActor) != arrow;
        if (drop) g_bowDropArrow = 0;
    }
    if (drop) g_arrowFall = {arrow, 0, 0.0f, Vec3{line.dir.x, 0, line.dir.z}};
    const Vec3 dir = line.dir;
    const int16_t sx = radians_to_s16(-std::asin(std::clamp(dir.y, -1.0f, 1.0f)));
    const int16_t sy = radians_to_s16(std::atan2(line.dir.x, line.dir.z));
    // ... only slid sideways onto the laser's line (the laser comes out of this arrow, on the left hand): a few units at
    // most, so the shot flies along the laser itself
    const Vec3 at = ld_vec(arrow + kPos), nock = line.head - line.dir * kArrowLength;
    const Vec3 onLine = nock + line.dir * dot(at - nock, line.dir);
    if (finite(onLine) && length(onLine - at) < 15.0f) {
        stf32(arrow + kPos, onLine.x);
        stf32(arrow + kPos + 4, onLine.y);
        stf32(arrow + kPos + 8, onLine.z);
    }
    st16(arrow + kShapeAngleY - 2, uint16_t(sx));
    st16(arrow + kShapeAngleY, uint16_t(sy));
    st16(arrow + kHeading - 2, uint16_t(int16_t(-sx)));  // current.angle.x
    st16(arrow + kHeading, uint16_t(sy));                 // current.angle.y
    if (ld32(link + kKeptEquipActor) != arrow)  // (let go of: this is its shot)
        LOG("[vr] bow: arrow %08X %s (direction %.2f %.2f %.2f)", arrow,
            drop ? "dropped: let go before drawn back, it falls from the hand" : "shot along its laser", double(dir.x),
            double(dir.y), double(dir.z));
}
uint32_t parry_widen(uint32_t link, uint32_t saved[3]) {
    if (!link || !first_person_play()) return 0;
    const uint32_t e = ld32(link + kLockOnActor);
    if (!e) return 0;
    const float dis = float(ldf32(e + kBtMaxDis)), start = float(ldf32(e + kBtStartFrame)), end = float(ldf32(e + kBtEndFrame));
    if (!std::isfinite(dis) || !std::isfinite(start) || !std::isfinite(end) || !(end > start)) return 0;
    saved[0] = ld32(e + kBtMaxDis);
    saved[1] = ld32(e + kBtStartFrame);
    saved[2] = ld32(e + kBtEndFrame);
    stf32(e + kBtMaxDis, dis * kParryReach);
    stf32(e + kBtStartFrame, start - kParryEarly);
    stf32(e + kBtEndFrame, end + kParryLate);
    return e;
}
void parry_unwiden(uint32_t enemy, const uint32_t saved[3]) {
    st32(enemy + kBtMaxDis, saved[0]);
    st32(enemy + kBtStartFrame, saved[1]);
    st32(enemy + kBtEndFrame, saved[2]);
}
// (2026-11-09, asked for: the game completes the attack on its own) the finishing cut no longer waits for a swing
constexpr bool kParryAutoCut = false;  // (false again since 2026-11-10, asked for: "after A is pressed then allow the player follow and execute the attack")
bool parry_cut_wait() {
    if (kParryAutoCut) return false;  // (the game's own cut, at once)
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!first_person_play() || !g_parry.inMove) return false;
    const uint64_t step = interp::logic_steps();
    // a full swing made in the move (not the one that began it, two steps before at the latest)
    const bool swung = g_parry.lastSwing > g_parry.moveStep + 2;
    const bool timedOut = step > g_parry.moveStep + kParryCutWait;
    if (swung || timedOut) {
        if (g_parry.waitLogged)
            LOG("[vr] parry: the finishing cut (%s)", swung ? "your swing" : "no swing in time: the game's own");
        g_parry.waitLogged = false;
        return false;
    }
    if (!g_parry.waitLogged) LOG("[vr] parry: the finishing cut waits for your swing");
    g_parry.waitLogged = true;
    return true;
}
bool keep_bow_hand(uint32_t link) {
    return running() && link && !g_thirdPerson.load() && aspect::gameplay_camera() &&
           bow_aim_proc(ld32(link + kCurProc));  // (the land's 0x94 and the boat's 0x8D: the game hides it in both)
}
bool aim_body_straight(uint32_t link) {
    return running() && link && !g_thirdPerson.load() && aspect::gameplay_camera() &&
           mods::camera_mouse::aiming_item(ld32(link + kCurProc));
}
// dCamera_c::CalcSubjectAngle (02506964) gives the first-person camera's pitch and yaw from the sticks: the yaw is
// Link's (work m3BA) plus yawMax x the yaw input (m384), the pitch pitchMax x the pitch input (m388), both inputs held
// in -1..1. Aiming items (bow, hookshot, boomerang, grappling hook: mods::camera_mouse::aiming_item) shoot along that
// camera's view, so the game's own sight (its target point and lock), projectiles and checks follow from it.
constexpr uint32_t kCamWork = 0x37C, kCamYawInput = kCamWork + 0x0C, kCamPitchInput = kCamWork + 0x10,
                   kCamLinkYaw = kCamWork + 0x42, kCamViewCenter = 0x3C + 0x08, kCamViewEye = 0x3C + 0x14;
void subject_angle(uint32_t cam, uint32_t pitchAddr, uint32_t yawAddr) {
    static const bool trace = [] { const char* e = std::getenv("WWHD_VR_TRACE_AIM"); return e && *e == '1'; }();
    if (!cam || !pitchAddr || !yawAddr) return;
    const uint32_t link = g_link.link;
    const uint32_t proc = link ? ld32(link + kCurProc) : 0;
    if (trace) {
        static uint64_t last = ~0ull;
        const uint64_t step = interp::logic_steps();
        if (step != last && step % 10 == 0) {
            last = step;
            const Vec3 center = ld_vec(cam + kCamViewCenter), eye = ld_vec(cam + kCamViewEye);
            const Vec3 d = eye - center;
            LOG("[vr] aim trace: proc %X pitch %d yaw %d (Link %d), inputs %.3f %.3f; center->eye yaw %d pitch %d",
                proc, int(int16_t(ld16(pitchAddr))), int(int16_t(ld16(yawAddr))), int(int16_t(ld16(cam + kCamLinkYaw))),
                double(ldf32(cam + kCamYawInput)), double(ldf32(cam + kCamPitchInput)),
                int(radians_to_s16(std::atan2(d.x, d.z))), int(radians_to_s16(std::atan2(d.y, std::sqrt(d.x * d.x + d.z * d.z)))));
        }
    }
    if (!running() || !link || !mods::camera_mouse::aiming_item(proc) || g_thirdPerson.load()) return;  // third person:
                                                                                                         // the sticks aim
    // the game's own pitch limit for this item: its result over its pitch input (measured 12743 for the hookshot)
    static int pitchLimit = 12743;
    const float pitchInput = float(ldf32(cam + kCamPitchInput));
    const int gamePitch = int16_t(ld16(pitchAddr));
    if (std::fabs(pitchInput) > 0.2f && std::isfinite(pitchInput))
        pitchLimit = std::clamp(int(std::lround(std::fabs(gamePitch / pitchInput))), 0x800, 0x3F00);
    std::lock_guard<std::mutex> lock(g_mutex);
    const Config c = config();
    // the hand aiming the item (vr_sword.h off_hand_item, fixed_item_side; the bow: the arrow's, the left)
    const uint16_t aimItem = aimed_item(proc, ld16(link + 0x69B0));  // (vr_sword.h: the boat's boomerang in flight too)
    const int d = bow_in_hand(aimItem) ? 0 : aim_side(c, aimItem);
    if (!g_input.valid || !g_input.hand[d].active) return;  // not tracked: the game's (stick) aim
    Vec3 bowFrom, bowDir;
    const bool bow = bow_in_hand(ld16(link + 0x69B0)) && bow_aim(bowFrom, bowDir);  // the bow: from its arrow
    const Vec3 point = to_game_dir(g_map, bow ? bowDir : rotate(g_input.hand[d].aim.orientation, {0, 0, -1}));
    int16_t pitch = 0, yaw = 0;
    if (!pointing_aim(point, pitchLimit, pitch, yaw)) return;
    st16(pitchAddr, uint16_t(pitch));
    st16(yawAddr, uint16_t(yaw));
    // the game's own aim state follows (its inputs as the stick would have left them), so leaving the item or losing
    // tracking carries on from here instead of snapping back
    stf32(cam + kCamPitchInput, std::clamp(float(pitch) / float(pitchLimit), -1.0f, 1.0f));
    stf32(cam + kCamYawInput, 0.0f);
    // (once for each aiming procedure in a row: the boat's 0x8B to 0x8D show apart from the land's; it had logged only
    // the first of the session, the telescope's, so a player's log could not tell whether the boat's items were
    // pointed, 2026-11-06)
    static uint32_t loggedProc = 0;
    if (proc != loggedProc) {
        loggedProc = proc;
        LOG("[vr] aiming by pointing: proc %X, the %s hand (pitch limit %d)%s", proc, d ? "right" : "left", pitchLimit,
            (ld32(play() + kPlayPlayerStatus0) & kShipRide) ? " (on the boat)" : "");
    }
}

// ---- the aiming laser (docs/VR_AIMING.md, vr_laser.h)
// Link's sight (daPy_sightPacket_c at +0x58E8): shown (+4), locked (+5: the game takes what it aims at: a hookshot
// target, a grappling point, the boomerang's or the bow's target), and its point (+8) where the aim lands
constexpr uint32_t kSight = 0x58E8, kSightDraw = kSight + 4, kSightLock = kSight + 5, kSightPos = kSight + 8;
// how far ahead of the controller's aim pose each item's line starts (metres): the hookshot's nozzle, the bow's grip,
// the boomerang's and the grappling hook's front, as the game draws them in the sword hand
float laser_muzzle(uint16_t item) {
    switch (item) {
    case 0x2F: return 0.14f;                         // hookshot
    case 0x27: case 0x35: case 0x36: return 0.04f;  // the Hero's Bow (with fire and ice arrows, with light arrows)
    case 0x2D: return 0.08f;                         // boomerang
    case 0x25: return 0.06f;                         // grappling hook
    default: return 0.06f;
    }
}
constexpr float kLaserMissMetres = 15.0f;
constexpr float kLaserMinMetres = 1.0f;
constexpr float kSightHoldSeconds = 0.5f;  // the game's sight kept for the laser this long after it stops being drawn  // a sight point nearer ahead than this: drawn as a miss (a laser that short is not seen)  // the line's length when the game's sight is not shown (nothing aimed at)
void update_laser(const Config& c, const xr::Views* head) {
    LaserView v;
    const uint32_t l = g_link.link;
    // from the hand that aims the item (the bow: the arrow's, the left)
    // (the item its procedure aims, vr_sword.h aimed_item: the boat's boomerang keeps its hand while it flies)
    const uint16_t laserItem = l ? aimed_item(ld32(l + kCurProc), ld16(l + 0x69B0)) : 0;
    const int d = l ? (bow_in_hand(laserItem) ? 0 : aim_side(c, laserItem)) : dominant_side(c);
    // (the boat's boomerang: shown while it is held, hidden from its release until it is caught, vr_sail_view.h
    // boat_boomerang_flying; asked for, 2026-11-06: "it needs to be there already and disappear when it's released")
    const bool on = c.aimLaser && running() && head && head->valid && l && g_link.valid && g_input.valid &&
                    !g_thirdPerson.load() && mods::camera_mouse::aiming_item(ld32(l + kCurProc)) &&
                    !boat_boomerang_flying(ld32(l + kCurProc), ld16(l + 0x69B0)) &&
                    !gfx::display_gamepad_menu_open() && g_input.hand[d].active;
    // the boat's boomerang held: the hand's own line, never drawn to the game's sight. Its sight is far (48 m in the
    // headset's logs) and the laser drawn to it was not seen while held, where the hand's 15 m line was (2026-11-06)
    const bool handLineOnly = l && ld32(l + kCurProc) == kProcShipBoomerang;
    // the bow's: only with its arrow (spawned with it), from the arrow's middle along it
    Vec3 bowFrom, bowDir;
    bool onArrow = false;
    const bool isBow = on && bow_in_hand(ld16(l + 0x69B0));
    const bool bow = isBow && bow_aim(bowFrom, bowDir, &onArrow) && onArrow;
    if (on && (!isBow || bow)) {
        const xr::HandState& hand = g_input.hand[d];
        const Vec3 fwd = bow ? bowDir : rotate(hand.aim.orientation, {0, 0, -1});
        v.from = bow ? bowFrom : hand.aim.position + fwd * laser_muzzle(ld16(l + 0x69B0 /* mEquipItem */));
        v.eye = (head->eye[0].pose.position + head->eye[1].pose.position) * 0.5f;
        bool hit = false;
        // the game's sight, held for kSightHoldSeconds while it blinks off: the boat's boomerang draws it only on the
        // steps Link's body has caught up with the aim (procShipBoomerang: setBodyAngleToCamera), so it went on and off
        // several times a second while the hand moved, and the laser jumped between the sight and the miss line with its
        // dot: "the laser keeps disappearing", where the hookshot's sight, drawn every step, held still (2026-11-06)
        static Vec3 heldSight{};
        static uint16_t heldSightItem = 0;
        static auto heldSightAt = std::chrono::steady_clock::time_point{};
        static bool heldSightLock = false;
        const auto nowSight = std::chrono::steady_clock::now();
        const bool sightDrawn = ld8(l + kSightDraw) != 0;
        if (sightDrawn) {
            heldSight = ld_vec(l + kSightPos);
            heldSightItem = laserItem;
            heldSightAt = nowSight;
            heldSightLock = ld8(l + kSightLock) != 0;
        }
        const bool sightHeld = !sightDrawn && heldSightItem == laserItem &&
                               std::chrono::duration<float>(nowSight - heldSightAt).count() < kSightHoldSeconds;
        if ((sightDrawn || sightHeld) && !handLineOnly) {
            const Vec3 at = to_xr_point(g_map, heldSight);
            const Vec3 to = at - v.from;
            // (a sight point at the hand or behind it is no aim: the miss line instead. With the boomerang held on the
            // boat the laser was not seen, though shown: 2026-11-06. kLaserMinMetres)
            if (finite(at) && dot(to, fwd) > kLaserMinMetres && length(to) < 300.0f) {
                v.to = at;
                v.valid = sightDrawn ? ld8(l + kSightLock) != 0 : heldSightLock;
                hit = true;
            }
        }
        if (!hit) v.to = v.from + fwd * kLaserMissMetres;
        v.visible = finite(v.from) && finite(v.to) && finite(v.eye);
        // (diagnostics) every 2 s while shown: what the line is drawn to, and the game's sight
        {
            static auto lastLaserLog = std::chrono::steady_clock::time_point{};
            const auto now = std::chrono::steady_clock::now();
            if (v.visible && now - lastLaserLog > std::chrono::seconds(2)) {
                lastLaserLog = now;
                const Vec3 sightAt = to_xr_point(g_map, ld_vec(l + kSightPos));
                LOG("[vr] aiming laser: item 0x%X, action 0x%X, the %s hand, %.2f m long (%s); the game's sight %s, %.2f m "
                    "from the hand, %.2f m ahead of it", unsigned(laserItem), unsigned(ld32(l + kCurProc)),
                    d ? "right" : "left", double(length(v.to - v.from)), hit ? "to the sight" : "the miss line",
                    ld8(l + kSightDraw) ? "drawn" : "not drawn", double(length(sightAt - v.from)),
                    double(dot(sightAt - v.from, fwd)));
            }
        }
    }
    // (diagnostics) how often the game's sight went off and on while the laser was shown: once each 10 s
    {
        static bool sightWas = false;
        static int blinks = 0;
        static auto since = std::chrono::steady_clock::now();
        const bool sightNow = v.visible && l && ld8(l + kSightDraw) != 0;
        if (v.visible && sightNow != sightWas) ++blinks;
        sightWas = sightNow;
        if (std::chrono::steady_clock::now() - since > std::chrono::seconds(10)) {
            if (blinks) LOG("[vr] aiming laser: the game's sight went on or off %d times in 10 s (held %.1f s for the laser)",
                            blinks, double(kSightHoldSeconds));
            blinks = 0;
            since = std::chrono::steady_clock::now();
        }
    }
    static bool shown = false;
    if (v.visible != shown) {
        shown = v.visible;
        LOG("[vr] aiming laser %s", shown ? "shown" : "hidden");
    }
    set_laser(v);
}

// ---- the game's HUD in third-person play (docs/VR_HUD.md)
// The HUD keeps its place at the picture's edges (aspect.cpp); in VR the picture spans the headset's whole field, so
// those edges are outside what the lenses show (reported in the headset: the HUD was not in view). In normal play,
// first and third person, the edge HUD is drawn vr.hudInset of the way out instead (in first person the hearts are on
// the arm), at vr.hudSize of its size (in the headset it looked too large at the game's size). Events, menus and the
// telescope: unchanged.
bool third_person_view() { return g_thirdPerson.load(); }
bool boat_view() { return g_boatView.load(); }
bool link_hidden() { return running() && free_roam_hides_link(g_freeRoamView.load() ? ViewMode::FreeRoam : ViewMode::FirstPerson,
                                                              aspect::gameplay_camera()); }
bool conducting_view() { return running() && conducting(); }
// (the VR UI's overlay rule, vr_ui.h: the Picto Boxes' viewfinder overlay, with N_Mode_00 in its N_All_00 as the
// telescope's has, is shown while one is aimed, as the telescope's while looking through it)
bool telescope_looking() { return subject_zoom_view(); }

bool hud_layout(float& inset, float& size) {
    inset = size = 1;
    if (!running() || !aspect::gameplay_camera() || subject_zoom_view() || gfx::display_gamepad_menu_open()) return false;
    const Config c = config();
    inset = c.hudInset;
    size = c.hudSize;
    static float loggedInset = -1, loggedSize = -1;
    if (inset != loggedInset || size != loggedSize) {
        loggedInset = inset;
        loggedSize = size;
        LOG("[vr] HUD: drawn %.0f%% of the way to the view's edges at %.0f%% of its size", double(inset * 100.0f),
            double(size * 100.0f));
    }
    return true;
}

// test aid (no headset needed): WWHD_VR_TEST_BODY=step:degrees[:step2:degrees2...] turns a simulated head that many
// degrees from Link's facing at each logic step given; the same BodyFollow and the same write as in VR turn Link while
// he stands, logging his heading and action every 10 steps for 3 s after each turn (docs/VR_BODY_FOLLOW.md, "Tests")
void test_body_follow(uint32_t link) {
    static const char* spec = std::getenv("WWHD_VR_TEST_BODY");
    if (!spec || !link) return;
    struct Turn { unsigned long long step; float degrees; };
    static std::vector<Turn> turns = [] {
        std::vector<Turn> v;
        const char* e = spec;
        unsigned long long st;
        float d;
        int n = 0;
        while (std::sscanf(e, "%llu:%f%n", &st, &d, &n) == 2) {
            v.push_back({st, d});
            e += n;
            if (*e != ':') break;
            ++e;
        }
        return v;
    }();
    static BodyFollow body;
    static float head = 0;
    static bool seeded = false;
    static size_t next = 0;
    static uint64_t logUntil = 0;
    const uint64_t step = interp::logic_steps();
    const float heading = s16_to_radians(int16_t(ld16(link + kHeading)));
    const uint32_t proc = ld32(link + kCurProc);
    if (!seeded) {
        body.set(heading);
        head = heading;
        seeded = true;
    }
    if (next < turns.size() && step >= turns[next].step) {
        head = wrap_radians(head + turns[next].degrees * kPi / 180.0f);
        LOG("[vr] body test: step %llu: the head turns %+.0f degrees (Link's action %u)", (unsigned long long)step,
            double(turns[next].degrees), proc);
        ++next;
        logUntil = step + 90;
    }
    if (next == 0) {  // before the first turn the simulated head looks his way (a save loads, he settles)
        body.set(heading);
        head = heading;
    } else if (!body_follow_proc(proc)) {
        body.set(heading);  // the game turns him
    } else {
        body.update(head, kStepSeconds, config().bodyFollowDegrees * kPi / 180.0f);
        set_link_facing(link, radians_to_s16(body.yaw));
    }
    if (step <= logUntil && step % 10 == 0)
        LOG("[vr] body test: step %llu: Link faces %.1f degrees (the head %.1f, %s), action %u", (unsigned long long)step,
            double(heading * 180.0f / kPi), double(head * 180.0f / kPi), body.following ? "following" : "settled", proc);
}

// The body under the head (g_room): before Link's logic, your head's horizontal movement since the last step moves him
// by the same amount, so his body stays under the view. Only where that is sensible: first-person normal play
// (first_person_play: not third person, events, cutscenes, menus or the first-person item view), the view calibrated,
// the head tracked, and Link standing or walking on the ground (daPyProc_WAIT_e 4, FREE_WAIT_e 5, MOVE_e 6). Not while
// he crawls or crouches, climbs, hangs, swims, is on a ladder or the boat, is locked on, attacks, uses an item or is
// in the air: there the game places him, and the view is held within vr.maxHeadOffset of him as before. Written to his
// position before his logic, so his own collision (walls, ground) takes it as any move of his: he does not pass
// through walls; against one the origin still takes the whole move, and the view stays at his body. At most
// kRoomStepMetres a step: faster than walking, so it only shows as a glide when he catches up after one of those
// (crawling, climbing) or after a stall
constexpr float kRoomStepMetres = 0.08f;  // (2.4 m/s at 30 steps a second)

// ---- walls (VR fork, docs/VR_CONTROLS.md, "Leaning into walls"): the game's own line check (cBgS::LineCross, as its
// actors check sight lines) from Link's eyes towards where the head's lean puts the view, once a step before his
// logic. The room-scale move goes only as far as is free, so the tracking origin moves by what he takes (it took the
// whole move before: against a wall the view stayed at his body while the head went on, and the world slid with the
// head); and the view's lean is held that far short of the wall (HeadController::wall_lean), so leaning into one does not put the
// eyes through it. Without the game's functions in this build nothing is checked, as before.
const uint32_t kLineCross = GC_OPTIONAL(0x02008860);  // cBgS::LineCross(bgs, linChk): true on a hit
const uint32_t kLinChkCt = GC_OPTIONAL(0x02008FEC);   // cBgS_LinChk::ct (the base of dBgS_LinChk)
const uint32_t kLinChkSet = GC_OPTIONAL(0x024F1AFC);  // dBgS_LinChk::Set(chk, start, end, actor)
const uint32_t kLinChkDt = GC_OPTIONAL(0x02008B4C);   // cBgS_LinChk::~cBgS_LinChk(chk, 0)
constexpr uint32_t kPlayBgS = 0x12A0;                 // dComIfG_play_c::mBgS (from dComIfG_getGameInfo)
constexpr uint32_t kLinChkBytes = 0x6C, kLinChkEnd = 0x24 + 0xC;  // mLin.mEnd: the hit point after a hit
constexpr float kWallMargin = 6.0f;   // game units the eyes stay from a wall: the near plane (3) and the eyes' spacing (3)
// the free distance (game units) from `from` along `to - from`, or -1: nothing in the way, or no check in this build.
// dBgS_LinChk built as the game's actors build theirs on the stack (d_a_bb.h dBgS_LinChk_ct/_dt, that unit's
// vtables), in memory of its own; the guest's registers are put back after
float free_distance(Cpu* cpu, uint32_t link, Vec3 from, Vec3 to) {
    static const uint32_t vt[6] = {GD_OPTIONAL(0x10007C3C), GD_OPTIONAL(0x10007C5C), GD_OPTIONAL(0x10007C6C),
                                   GD_OPTIONAL(0x10007C4C), GD_OPTIONAL(0x10007BFC), GD_OPTIONAL(0x10007BEC)};
    if (!cpu || !kLineCross || !kLinChkCt || !kLinChkSet || !kLinChkDt || !finite(from) || !finite(to)) return -1;
    for (uint32_t v : vt) if (!v) return -1;
    static const uint32_t chk = mem::host_alloc(kLinChkBytes, 16), points = mem::host_alloc(24, 16);
    const Cpu saved = *cpu;
    guest_call(cpu, kLinChkCt, {chk});
    for (uint32_t i = 0; i < 7; ++i) st8(chk + 0x5C + i, 0);
    st32(chk + 0x68, 1);
    st32(chk + 0x04, chk + 0x64);
    st32(chk + 0x00, chk + 0x58);
    st32(chk + 0x10, vt[0]);
    st32(chk + 0x64, vt[1]);
    st32(chk + 0x58, vt[2]);
    st32(chk + 0x20, vt[3]);
    st_vec(points, from);
    st_vec(points + 12, to);
    guest_call(cpu, kLinChkSet, {chk, points, points + 12, link});
    const bool hit = guest_call(cpu, kLineCross, {guest_call(cpu, kGameInfo) + kPlayBgS, chk}) & 0xFF;
    const Vec3 at = ld_vec(chk + kLinChkEnd);
    st32(chk + 0x58, vt[2]);
    st32(chk + 0x64, vt[4]);
    st32(chk + 0x20, vt[5]);
    guest_call(cpu, kLinChkDt, {chk, 0});
    *cpu = saved;
    if (!hit || !finite(at)) return -1;
    return length(Vec3{at.x - from.x, 0, at.z - from.z});
}
// Link's eye point now (before his logic: his position, the eye point of the last step)
Vec3 eyes_now(uint32_t link) {
    return ld_vec(link + kPos) + g_link.eyeOffset + Vec3{0, g_link.eyeHeight, 0};
}
// How far the view may lean from the tracking origin (metres) until the next step: the head's lean now from the origin,
// checked from his eyes, plus the margin; no wall in the way: no limit but vr.maxHeadOffset (g_mutex held)
void wall_lean_update(uint32_t link, Cpu* cpu, const xr::Views& v, float scale) {
    g_head.set_wall_lean(1e9f);
    Vec3 lean = v.head.position - g_head.origin_now();
    lean.y = 0;
    const float metres = length(lean);
    if (!(metres > 0.01f) || !(scale > 0)) return;
    const Vec3 dir = to_game_dir(g_map, lean * (1.0f / metres));
    const Vec3 from = eyes_now(link);
    const float free = free_distance(cpu, link, from, from + dir * (metres * scale + kWallMargin));
    if (free >= 0) g_head.set_wall_lean(std::max(0.0f, free - kWallMargin) / scale);
}

void room_scale_move(uint32_t link, Cpu* cpu) {  // g_mutex held
    const Config c = config();
    g_head.begin_move();  // (nothing turned about his eyes yet, no wall known)
    if (!g_head.calibrated_for(link) || !g_link.valid || link != g_link.link) return;
    if (!first_person_play()) return;
    xr::Views v;
    if (!game_views(display_time(), v) || !finite(v.head.position)) return;
    const float scale = g_map.unitsPerMetre > 0 ? g_map.unitsPerMetre : world_scale();
    // (the view's limit at walls in every first-person state, also where the game places him: crawling, climbing...)
    struct WallLean {
        uint32_t link; Cpu* cpu; const xr::Views& v; float scale;
        ~WallLean() { if (g_link.offsetSeeded) wall_lean_update(link, cpu, v, scale); }
    } wallLean{link, cpu, v, scale};
    const uint32_t proc = ld32(link + kCurProc);
    if (proc != 4 && proc != 5 && proc != 6) return;
    // Standing, the body faces the head every step (the frame update keeps g_body.yaw on the head's yaw) and link_executing
    // has just turned him to it. Turned about his feet, his eyes (about 18 units in front of them) swung round with
    // every turn, and the view's eye point was held through the turn and glided after it: in a full turn of the head
    // the body only lined up with the view once the head stopped (reported in the headset). He is turned about his
    // eyes instead: his position moves by what the turn swings his eye point, so his eyes stay under the view. The
    // view is not moved by it (his eye point stays where it was), and the tracking origin is not either. This turn is
    // made in every first-person mode (seated too); only the walk below needs vr.roomScale
    if (body_follows_head(link) && g_link.offsetSeeded && finite(g_link.eyeOffset)) {
        const float before = g_link.heading, after = link_facing(g_map.heading, g_body.yaw);
        const Vec3 e = g_link.eyeOffset;
        const Vec3 turned = heading_right(after) * dot(e, heading_right(before)) + heading_dir(after) * dot(e, heading_dir(before));
        const Vec3 about = Vec3{e.x - turned.x, 0, e.z - turned.z};
        const Vec3 pos = ld_vec(link + kPos);
        if (finite(about) && finite(pos) && length(about) < 40.0f) {
            set_link_xz(link, pos.x + about.x, pos.z + about.z);
            g_head.turned_about_eyes(about, after);
        }
    }
    if (!c.roomScale) return;
    Vec3 move = v.head.position - g_head.origin_now();
    move.y = 0;  // (crouching and standing up move the view, not Link: his height is the game's)
    const float metres = length(move);
    if (!(metres > 1e-4f)) return;
    const bool catchUp = metres > kRoomStepMetres;
    if (catchUp) move = move * (kRoomStepMetres / metres);
    if (g_head.set_catching_up(catchUp)) {
        if (catchUp) LOG("[vr] body under the head: Link catches up %.2f m (action 0x%X)", double(metres), proc);
    }
    Vec3 game = to_game_dir(g_map, move) * scale;
    const Vec3 pos = ld_vec(link + kPos);
    if (!finite(pos) || !finite(game)) return;
    // (walls: only as far as is free from his eyes; the origin takes the same, so the view keeps the rest of the lean)
    if (g_link.offsetSeeded) {
        const Vec3 from = eyes_now(link);
        const float want = length(game);
        const float free = want > 0 ? free_distance(cpu, link, from, from + game * ((want + kWallMargin) / want)) : -1;
        if (free >= 0 && free - kWallMargin < want) {
            const float k = std::max(0.0f, free - kWallMargin) / want;
            game = game * k;
            move = move * k;
            if (k <= 0) return;
        }
    }
    set_link_xz(link, pos.x + game.x, pos.z + game.z);
    g_head.walk(move, game, Vec3{pos.x + game.x, pos.y, pos.z + game.z}, proc == 4 || proc == 5);
    if (g_link.eyeHold) g_link.eyeHoldPoint = g_link.eyeHoldPoint + game;  // (held in the world: moved with him)
}
// walking backwards (filter_pad_controllers, g_backStep): Link stands, facing where your body faces (set just before),
// and is moved the stick's way from where the head looks, at a full push as fast as he runs forward (his
// mMaxNormalSpeed, back_slide_speed; the game gets no stick meanwhile). Walls as for the room-scale walk: only as far as is free from his eyes. His height and
// what is under him stay the game's (ledges: he falls as from a walk). The view comes with him: the tracking space is
// anchored at his eyes. No walking animation (not seen in first person: asked for in the headset)
// (until 2026-11-04 a fixed 6 units a step, 1.8 m/s, about a third of his run: walking backwards was slow, reported in
// the headset) Link's top speed running forward, in game units a logic step: daPy_py_c::mMaxNormalSpeed (HD 0x3C4,
// mods/traversal.cpp), the speed a full push of the stick runs him at. The run/swim boost is not applied (it speeds
// only his running procedures, which he is not in here)
constexpr uint32_t kMaxNormalSpeed = 0x3C4;
constexpr float kBackSlideFallback = 17.0f;  // (the stock mMaxNormalSpeed, should the field read wrong)
float back_slide_speed(uint32_t link) {
    const float v = float(ldf32(link + kMaxNormalSpeed));
    return std::isfinite(v) && v >= 3.0f && v <= 60.0f ? v : kBackSlideFallback;
}
void back_slide(uint32_t link, Cpu* cpu) {  // g_mutex held
    // (the controllers' input lost or play changed since the pad read: that read's slide is not kept)
    if ((!g_backStep && !g_aimStep) || !g_input.valid || !first_person_play() || !g_link.valid ||
        link != g_link.link || !g_link.offsetSeeded)
        return;
    const uint32_t proc = ld32(link + kCurProc);
    // (walking backwards: standing only, walking he stops first, the game having no stick; aiming: its aims)
    if (g_aimStep ? !mods::camera_mouse::aiming_item(proc) : (proc != 4 && proc != 5)) return;
    Vec3 game = to_game_dir(g_map, g_backSlide) * back_slide_speed(link);
    game.y = 0;
    const float want = length(game);
    const Vec3 pos = ld_vec(link + kPos);
    if (!(want > 1e-3f) || !finite(game) || !finite(pos)) return;
    const Vec3 from = eyes_now(link);
    const float free = free_distance(cpu, link, from, from + game * ((want + kWallMargin) / want));
    if (free >= 0 && free - kWallMargin < want) {
        const float k = std::max(0.0f, free - kWallMargin) / want;
        if (k <= 0) return;
        game = game * k;
    }
    set_link_xz(link, pos.x + game.x, pos.z + game.z);
    if (g_link.eyeHold) g_link.eyeHoldPoint = g_link.eyeHoldPoint + game;  // (held in the world: moved with him)
}

void link_executing(uint32_t link, Cpu* cpu) {
    GameWorkTimer workTimer(GameWork::Execute);  // (docs/VR_PERFORMANCE.md: the VR work on the game thread)
    test_body_follow(link);
    // the body follows the head (docs/VR_BODY_FOLLOW.md): while Link stands in first-person play he faces where the
    // body faces (and while he conducts), set before his logic so his animation, model, arms and what he holds all
    // turn with him this step
    if (running() && link) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_head.calibrated() && g_link.valid && link == g_link.link && body_follows_head(link)) {
            set_link_facing(link, radians_to_s16(link_facing(g_map.heading, g_body.yaw)));
        }
        room_scale_move(link, cpu);
        back_slide(link, cpu);
    }
    // the physical sword's capsules: its hits of the last collision check (haptics), then unarmed, so that Link's
    // logic sees no attack of its own (link_executed arms them again while the hand swings)
    if (g_physicalLink && g_physicalLink == link) {
        const Config c = config();
        std::lock_guard<std::mutex> lock(g_mutex);
        bool hit = false;
        for (int i = 0; i < 3; ++i) {
            const uint32_t cps = link + kAtCps + i * kCpsStride;
            if ((ld32(cps) & 1) && (ld32(cps + 4) & 1) && g_ledger.record(ld32(cps + 8))) hit = true;
            st32(cps, ld32(cps) & ~1u);
        }
        if (hit && running()) session().haptic(dominant_side(c), c.hapticStrength, 0.06f);
    }
    g_physicalLink = 0;
    // the game's held item and hand indices back before his logic runs (apply_hands set them for the frames drawn)
    if (g_handsLink && g_handsLink == link) {
        st16(link + kEquipItem, g_savedItem);
        st8(link + kLeftHandIdx, g_savedHands[0]);
        st8(link + kRightHandIdx, g_savedHands[1]);
    }
    g_handsLink = 0;
    // the telescope, the Wind Waker or a Picto Box put away once its use ends in first person (vr_sword.h
    // PutAwayAfterUse): the game's own deleteEquipItem, with its put-away sound, before his logic sees the item
    if (running() && link && link == g_link.link && g_link.valid) {
        static PutAwayAfterUse putAway;
        const uint16_t item = ld16(link + kEquipItem);
        const uint32_t proc = ld32(link + kCurProc);
        const bool inUse = proc == 0 || proc == 0x9A || proc == 0x9B || conducting() || picto_aim() ||
                           (ld32(play() + kPlayPlayerStatus0) & kTelescopeLook);  // (procScope 0, TactWait / TactPlay)
        const bool idle = first_person_play() && proc >= 4 && proc <= 7;
        if (putAway.update(item, inUse, idle)) {
            const Cpu saved = *cpu;
            guest_call(cpu, kDeleteEquipItem, {link, 1});
            *cpu = saved;
            LOG("[vr] put away: item 0x%02X after its use (action 0x%X), now 0x%X", unsigned(item), unsigned(proc),
                unsigned(ld16(link + kEquipItem)));
        }
    }
}

void link_draw_end(uint32_t link) {
    GameWorkTimer workTimer(GameWork::Draw);  // (docs/VR_PERFORMANCE.md: the VR work on the game thread)
    if (running() && link) hookshot_tip_end(link);
    if (g_statusAddr) st32(g_statusAddr, g_statusSaved);
    g_statusAddr = 0;
}

// ---- what Link holds over his head (docs/VR_HELD_ITEMS.md, "Pots and barrels")
bool hides_held_object(uint32_t actor) {
    constexpr uint32_t kActorStatus = 0x2E0, kCarryNow = 0x2000;  // fopAcM_checkCarryNow
    if (!actor || !running() || !first_person_play()) return false;
    const uint32_t link = g_link.link;
    if (!link || !g_link.valid || ld32(link + kKeptGrabActor) != actor) return false;
    if (ld32(link + kKeptEquipActor) == actor) return false;  // the stick he holds as a weapon (it is kept as grabbed too)
    if (!(ld32(actor + kActorStatus) & kCarryNow)) return false;  // let go: drawn again
    if (carried_bomb(link) == actor) return false;  // a bomb: in the hand, seen (link_executed)
    static uint32_t logged = 0;
    if (logged != actor) {
        logged = actor;
        LOG("[vr] held overhead: actor %08X hidden in first person while Link carries it", actor);
    }
    return true;
}

// daBoko_Draw of `actor`, before the original: the weapon Link holds placed at the tracked hand as his draw placed it
void weapon_drawing(uint32_t actor) {
    GameWorkTimer workTimer(GameWork::Draw);  // (docs/VR_PERFORMANCE.md: the VR work on the game thread)
    if (!actor || !running()) return;
    const uint32_t link = g_link.link;
    if (!link || !g_link.valid || ld32(link + kKeptEquipActor) != actor || !g_heldTarget.valid) return;
    const uint32_t model = ld32(actor + kBokoModel);
    if (model != g_heldTarget.model) return;
    // its joints are in its base's frame (its own calculation from the base Link's setItemModel set) or, drawn again
    // in a later pass of the same step, in the frame this put them in: moved from that frame to the tracked one
    uint16_t count = 0;
    const uint32_t mtx = matrices_of(model, count);
    if (!mtx || count > 32) return;
    const Joint root = read_joint(mtx, 0);
    Quat fromRot;
    Vec3 fromAt;
    if (same(root.pos, g_held.rootPlaced)) {
        fromRot = g_held.placedRot;
        fromAt = g_held.placedAt;
    } else {
        const Joint base = read_joint(model + kModelBaseMtx, 0);
        if (!finite(base.pos)) return;
        fromRot = base.rot;
        fromAt = base.pos;
    }
    move_model(model, fromAt, g_heldTarget.at, normalize(g_heldTarget.rot * conjugate(fromRot)));
    g_held.placedRot = g_heldTarget.rot;
    g_held.placedAt = g_heldTarget.at;
    const Joint placedRoot = read_joint(mtx, 0);
    g_held.rootPlaced = placedRoot.pos;
    // its root joint in its base's frame (frame_correct finds the base its blended joints are in from it)
    g_weaponGrip.rootRot = normalize(conjugate(g_heldTarget.rot) * placedRoot.rot);
    g_weaponGrip.rootPos = rotate(conjugate(g_heldTarget.rot), placedRoot.pos - g_heldTarget.at);
    g_weaponGrip.rootValid = finite(placedRoot.pos);
    static int logged = 0;
    if (logged < 3) {
        ++logged;
        LOG("[vr] held weapon: its own draw %s Link's in this frame (placed at the tracked hand again)",
            g_linkDrawnFrame == render::frame_count() ? "after" : "before");
    }
}

}  // namespace vr::game

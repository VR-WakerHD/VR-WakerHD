// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// VR mode, game thread (docs/VR_ARCHITECTURE.md): the head-tracked camera, the controllers as the GamePad,
// Link's first-person presentation and the tracked sword in the game's own attack collision.
// Called from existing hooks: interp.cpp (camera_draw), mods/camera.cpp (daPy_Execute, the pad filter) and
// mods/vr_hooks.cpp (daPy_Draw). Every function does nothing unless a VR session runs.
#pragma once
#include <cstdint>

#include "input.h"

struct Cpu;

namespace vr::game {

// camera_draw (024FFC40) of camera `cam`, around the original function: the drawn camera becomes the head
class CameraOverride {
public:
    explicit CameraOverride(uint32_t cam);
    ~CameraOverride();
    CameraOverride(const CameraOverride&) = delete;
    CameraOverride& operator=(const CameraOverride&) = delete;

private:
    uint32_t cam_ = 0;
    bool written_ = false;
    uint32_t saved_[10] = {};  // eye, center, up (9 words) and the bank (s16 in the 10th)
};

// daPy_Execute (0240EBB0) of Link, after the original: Link's position for the camera anchor, the sword's
// attack capsules from the tracked blade while the game's attack is active or the hand swings (the physical sword:
// they are armed and entered into the game's collision with `c`), hit haptics
void link_executed(uint32_t link, Cpu* c);
// fopAc_Draw of `actor`: true while it is what Link holds over his head (a pot, a barrel: mActorKeepGrab) in
// first-person play, where it covered the view. The caller then sets the game's one-shot no-draw bit, which the
// draw clears again: the moment he lets go (thrown, put down, broken), it is drawn as before
bool hides_held_object(uint32_t actor);
// daBoko_Draw (020C0E04) of `actor`, before the original: the weapon Link holds (a stick picked up from an enemy) is
// placed at the tracked hand as his draw placed it (its draw may come before his)
void weapon_drawing(uint32_t actor);
// before the original daPy_Execute: what VR changed for Link's draw only is put back before his logic runs
void link_executing(uint32_t link, Cpu* cpu);
// end of input::read(): the controllers' buttons and sticks as the GamePad's, swings as the sword button
void filter_pad(input::PadState& s);
// the VR menu (docs/VR_MENU.md): open (the left stick's click toggles it), and the pause it holds: true before a
// pass while the game is paused (interp.cpp draws the pass without logic)
bool menu_open();
bool menu_pause_pass();
// the new save's name is asked (the system keyboard on the title or file select, docs/VR_MENU.md): its keyboard shows on
// the headset's panel and the controllers drive it
bool name_entry_open();
// daPy_Draw (023DBBE4) around the original: the first-person presentation (head hidden)
void link_draw_begin(uint32_t link);
void link_draw_end(uint32_t link);
// Link is not drawn at all (his model, what he holds, his shadow): the Free Roam Camera in normal play (vr_sail_view.h
// free_roam_hides_link, asked for 2026-11-06). His logic runs as always
bool link_hidden();
// recentre now (settings overlay; also the controllers' gesture)
void request_recenter();
// dCam_getControledAngleY (024F8018), after the original: in VR gameplay the heading forward on the stick means
int16_t controlled_angle_hook(int16_t original);
// daPy_lk_c::jointCB1 (02403150), after the original: Link's arms posed towards the controllers
void link_arms(uint32_t link);
// dCamera_c::CalcSubjectAngle's result (pitch and yaw, s16 at the guest addresses): an aimed item aims where the sword
// hand's controller points (docs/VR_AIMING.md)
void subject_angle(uint32_t camera, uint32_t pitchAddr, uint32_t yawAddr);
// daPy_lk_c::jointBeforeCB, his spine (joint 2): true while it should not bend to the aim (VR first person, an item
// aimed by pointing). The game bends it by mBodyAngle, the aim the arrows and hooks fly along; the hook leaves that
// value as it is and only poses the spine without it (docs/VR_AIMING.md, "The body")
bool aim_body_straight(uint32_t link);
// daPy_sightPacket_c::draw (02414834): the size the game's own aiming sight (the red mark where the bow, hookshot or
// grappling hook will hit) is drawn at in VR first person (1/16), 1 otherwise (asked for in the headset: too big)
float sight_scale();
// daPy_lk_c::setDrawHandModel (023D8624): true while Link's left hand is to stay drawn with the bow aimed (VR first
// person, procedure 0x94 on land or 0x8D on the boat: the HD game hides it there before an arrow is on the bow)
bool keep_bow_hand(uint32_t link);
// daPy_lk_c::procBtJumpCut_init / procBtRollCut_init / procBtVerticalJumpCut_init (0242FA80, 02430078, 024306C8): true
// while a parry's finishing cut waits for the player's own swing (VR first person; docs/VR_CONTROLS.md, "Parry
// attacks"); the game calls them again each step, so the cut follows the swing, or the timeout, or no cut on landing
bool parry_cut_wait();
// daPy_lk_c::setSpecialBattle (023EC370, the parry's prompt) and changeSpecialBattle (023E5400, the parry's start): in VR
// first person the locked-on enemy's parry window is widened for the call (earlier, later, farther; docs/VR_CONTROLS.md,
// "Parry attacks"). Returns the enemy (0: nothing widened); parry_unwiden puts its own values back after the call
uint32_t parry_widen(uint32_t link, uint32_t saved[3]);
void parry_unwiden(uint32_t enemy, const uint32_t saved[3]);
// first-person play with a bomb in the hand (docs/VR_HELD_ITEMS.md, "Bombs in the hand"): its action prompt hidden
bool bomb_in_hand();
// daArrow_c::setKeepMatrix (0205244C), after the game's: the arrow on the bow (VR first person) given the place and
// angles of the line it is drawn along (frame_correct), which its shot flies along
void arrow_kept(uint32_t arrow);
// J3DModel::viewCalc (interp.cpp), each pass, for every model it draws: Link's arms aimed at the controllers as they are
// this frame (docs/VR_ARM_ANIMATION.md, "Hands every frame"): his body's arms re-solved from the drawn pose to where
// the controllers are now, and his hands model and what he holds moved with the hands. `w` is the model's joint
// matrices as drawn this pass (big-endian guest words, 12 per joint), `t` the step blend they are at. True when it
// changed them
bool frame_correct(uint32_t model, uint32_t n, uint32_t* w, float t);
// the game's screen blur (interp_fx.cpp hook_02732D3C, docs/VR_SCREEN_EFFECTS.md): true while it is not drawn in the
// headset (vr.screenBlur off); and the map from this frame's picture to the last one's, by the view's turn between the
// two (vr_camera.h blur_reprojection), false when there is none (no VR view, a cut)
bool screen_blur_off();
bool blur_head_turn(float a[6]);
// the game's depth of field (interp_fx.cpp hook_027824A8, docs/VR_SCREEN_EFFECTS.md): true while its blur is not drawn
// in the headset (vr.depthOfField off)
bool depth_of_field_off();

}  // namespace vr::game

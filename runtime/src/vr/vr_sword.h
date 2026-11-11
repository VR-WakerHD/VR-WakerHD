// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// The VR sword's state and the shoulder draw (docs/VR_HELD_ITEMS.md, "Drawing and sheathing").
//
// The game owns the sword: whether it is in Link's hand (its model on his hand joint, vr_game.cpp measure_held) and
// whether an attack is set (his attack capsules). The VR side keeps one state derived from those, plus the draw it
// asked for, and gates on it: real swings attack only while the sword is held, the attack button does nothing while
// it is sheathed (the shoulder draw is the way to draw it), and the draw's own slash never hits.
#pragma once
#include <algorithm>
#include <cmath>
#include "vr_math.h"

namespace vr {

// Link's held melee weapons the tracked sword handles (mEquipItem, daPy_lk_c +0x69B0): the sword (daPyItem_SWORD_e
// 0x103) and a weapon picked up from an enemy, the stick among them (daPyItem_BOKO_e 0x101). The game keeps each
// one's own rules: its cuts branch on the item, and a picked-up weapon's power, sound and hit mark come from its actor
// (setEnemyWeaponAtParam)
constexpr uint16_t kItemSword = 0x103, kItemEnemyWeapon = 0x101;
// and the Skull Hammer (dItemNo_SKULL_HAMMER_e 0x33, docs/VR_SKULL_HAMMER.md): held, turned and swung by the sword hand
// the same way, its attacks the game's own (its item button, its swings' power, cut types and ground impact)
constexpr uint16_t kItemSkullHammer = 0x33;
inline bool melee_item(uint16_t item) { return item == kItemSword || item == kItemEnemyWeapon || item == kItemSkullHammer; }
// the GamePad button a full swing presses to attack with it: B, the sword button, for the sword; A for a picked-up
// weapon. In the HD game the do button swings a Bokoblin's weapon and the sword button throws it
// (d_a_player_main_03.cpp: procWeaponNormalSwing_init on A, procWeaponThrow_init on B); B threw the stick at every
// full swing (reported in the headset)
inline bool swing_presses_do_button(uint16_t item) { return item == kItemEnemyWeapon; }
// the physical swing's attack for that weapon (its hits along the tracked blade, as the sword's): 1 the sword's basic
// slash (setNormalCutAtParam), 2 a picked-up weapon's own power, sound and hit mark (setEnemyWeaponAtParam(FALSE), per
// its type from its actor), 0 none (its actor gone: the game attacks with none either, procJumpCut_init). (Turned off
// for a while: every throw of the stick in the headset came with a physical cut, but the throws were the sword button,
// B, which throws it: swing_presses_do_button)
inline int melee_attack(uint16_t item, uint32_t keptActor) {
    if (item == kItemSword) return 1;
    if (item == kItemEnemyWeapon) return keptActor ? 2 : 0;
    return 0;  // the hammer: none without the game's own swing (its hits need its cut type: hammer switches, pegs)
}

// ---- the aiming items' hands (docs/VR_AIMING.md): which hand holds and aims each, and whose trigger uses it
constexpr uint16_t kItemGrapplingHook = 0x25, kItemHookshot = 0x2F, kItemBoomerang = 0x2D, kItemDekuLeaf = 0x34;
inline bool bow_in_hand(uint16_t item) { return item == 0x27 || item == 0x35 || item == 0x36; }  // with its arrows
// held and aimed by the off hand (asked for in the headset: their lasers and aim on the off hand, where they are held):
// the grappling hook, and the hookshot and boomerang in their original places (the game holds them in Link's left hand,
// the off hand in right-handed play). The bow is in the sword hand (vr_bow.h)
inline bool off_hand_item(uint16_t item) { return item == kItemGrapplingHook || item == kItemHookshot || item == kItemBoomerang; }
// the trigger that uses an aimed item (its item button pressed and held with it, as on the GamePad): the grappling
// hook and the boomerang the off hand's (asked for in the headset: the left trigger; the boomerang also by a downward
// swing of that hand); the hookshot the sword hand's (asked for: the right trigger). The bow has its own: either trigger shoots the arrow on it (vr_bow.h)
inline bool fired_by_trigger(uint16_t item) { return !bow_in_hand(item); }
inline bool fired_by_off_trigger(uint16_t item) { return item == kItemGrapplingHook || item == kItemBoomerang; }
// The bow, the hookshot, the grappling hook, the boomerang and the Deku Leaf in the same physical hands in left- and
// right-handed play (asked for in the headset): the bow in the right hand (its arrow put on with the left), the
// others in the left (the game holds the leaf in Link's left hand too: daPy_lk_c setItemModel); 0 the left controller,
// 1 the right, -1 by the handedness (the others)
inline int fixed_item_side(uint16_t item) {
    if (bow_in_hand(item)) return 1;
    if (item == kItemHookshot || item == kItemGrapplingHook || item == kItemBoomerang || item == kItemDekuLeaf) return 0;
    return -1;
}
// the grappling hook, the boomerang and the Deku Leaf are used with the physical left trigger in either handedness
// (fixed_item_side; the leaf also by a quick up or down swing of the left hand: vr_swing.h FlapDetector)
inline bool fired_by_left_trigger(uint16_t item) {
    return item == kItemGrapplingHook || item == kItemBoomerang || item == kItemDekuLeaf;
}
// ...and the hookshot by either trigger (asked for in the headset: the left one too)
inline bool fired_by_both_triggers(uint16_t item) { return item == kItemHookshot; }
// Third person (the game's camera; on land and on the boat), the bow out: either trigger is the bow's item button
// while it is held (the string drawn), let go it shoots, as the game's button does (asked for, 2026-11-06: "let left
// or right trigger fire off arrows when Link uses the bow"). Their own buttons (ZL's lock-on) are not given meanwhile.
// First person has VR archery instead (vr_bow.h). `playNow`: normal play, no menu
inline bool triggers_fire_bow(bool thirdPerson, bool playNow, uint16_t item) {
    return thirdPerson && playNow && bow_in_hand(item);
}
// The item an aiming procedure aims, for its hand (aim_side) and its laser: the boomerang's and the hookshot's own
// procedures are theirs whatever mEquipItem reads. On the boat Link stays in SHIP_BOOMERANG (0x8B) while the boomerang
// flies, with mEquipItem no longer the boomerang, and the laser came from the sword hand then (reported, 2026-11-06:
// "the red laser switches to the other hand"). Land: BOOMERANG_SUBJECT/MOVE 0x80/0x81, HOOKSHOT_SUBJECT/MOVE 0x83/0x84;
// the boat: 0x8B, 0x8C
inline uint16_t aimed_item(uint32_t proc, uint16_t equipItem) {
    if (proc == 0x80 || proc == 0x81 || proc == 0x8B) return kItemBoomerang;
    if (proc == 0x83 || proc == 0x84 || proc == 0x8C) return kItemHookshot;
    return equipItem;
}

// ---- the Skull Hammer (docs/VR_SKULL_HAMMER.md)
// The game swings it from the item button it is on (daPy_lk_c::checkNextMode): with the left stick pushed to Link's
// side a side swing (procHammerSideSwing_init, 0x51, cut type 0x11), else a front swing (its windup 0x52, the swing
// 0x53 with the ground check and quake, its end 0x54; cut type 0x12). Its hit capsules are the sword's three
// (setSwordAtCollision) from mSwordTopPos and m36C4, which daPy_lk_c::execute takes from the hammer's base matrix
// (l_execute_hammer_top (160, 70, 0): the face; l_execute_hammer_root (8, 0, 0): at the hand; its shaft along +X).
constexpr uint32_t kProcHammerSideSwing = 0x51, kProcHammerFrontReady = 0x52, kProcHammerFrontSwing = 0x53,
                   kProcHammerFrontEnd = 0x54;
inline bool hammer_swing_proc(uint32_t proc) { return proc >= kProcHammerSideSwing && proc <= kProcHammerFrontEnd; }
// the head's centre (on the shaft's line at the face's distance along it), in the hammer's base frame: the tracked
// collision runs from here to the face, so the handle and the hand do not hit
constexpr float kHammerHeadAlong = 160.0f;
// the item button (0 X, 1 Y, 2 R) whose item (the play's mSelectItem) is `item`; -1: on none
inline int item_button(const uint8_t items[3], uint8_t item) {
    for (int i = 0; i < 3; ++i)
        if (items[i] == item) return i;
    return -1;
}
// a full swing's left stick for the hammer: sideways (to the side the swing goes, in the body's frame) for a horizontal
// swing, which the game makes a side swing; none for the others (a front swing: downward, overhead)
inline float hammer_swing_stick(bool horizontal, int side) { return horizontal ? (side >= 0 ? 1.0f : -1.0f) : 0.0f; }
// The windup the tracked swing has already made. The game's front swing waits for its windup animation (frames 2 to 28
// at rate 1: 0.9 s) and its side swing hits from frame 25 (at rate 1.2 from 4: 0.6 s), so a swing that pressed the
// button would hit about a second after the hammer came down. For a swing the hand made, the windup's animation is
// ended (its rate 0: procHammerFrontSwingReady goes on to the swing at its next step) or the side swing is moved to
// just before its hits (frame 24). The procedures, their attack parameters, cut types, sounds and checks all still run.
struct WindupSkip {
    bool apply = false;
    bool endAnimation = false;  // set the rate to 0 (the windup's end)
    float frame = 0;            // else: move to this frame
};
constexpr float kHammerSideHitFrame = 24.0f;
inline WindupSkip hammer_windup_skip(uint32_t proc, float frame) {
    WindupSkip s;
    if (proc == kProcHammerFrontReady) {
        s.apply = s.endAnimation = true;
    } else if (proc == kProcHammerSideSwing && std::isfinite(frame) && frame < kHammerSideHitFrame) {
        s.apply = true;
        s.frame = kHammerSideHitFrame;
    }
    return s;
}

enum class SwordState { Sheathed, Drawing, Held, Attacking, Sheathing };

inline const char* sword_state_name(SwordState s) {
    switch (s) {
    case SwordState::Sheathed: return "sheathed";
    case SwordState::Drawing: return "drawing";
    case SwordState::Held: return "held";
    case SwordState::Attacking: return "attacking";
    case SwordState::Sheathing: return "sheathing";
    }
    return "?";
}

struct SwordMachine {
    SwordState state = SwordState::Sheathed;
    float timer = 0;      // in Drawing / Sheathing
    float drawnFor = 9;   // seconds since a requested draw reached the hand (the draw's slash: no hits)
    static constexpr float kDrawTimeout = 1.5f;   // a draw that did not reach the hand gives up
    static constexpr float kSheathTime = 0.5f;    // leaving the hand settles to sheathed
    static constexpr float kNoHitAfterDraw = 0.6f;

    // one logic step: the game's facts and whether a draw was asked for
    SwordState update(bool inHand, bool attacking, bool drawRequested, float dt) {
        timer += dt;
        drawnFor += dt;
        switch (state) {
        case SwordState::Sheathed:
            if (inHand) set(SwordState::Held);  // drawn another way (an event, the game itself)
            else if (drawRequested) set(SwordState::Drawing);
            break;
        case SwordState::Drawing:
            if (inHand) { set(SwordState::Held); drawnFor = 0; }
            else if (timer > kDrawTimeout) set(SwordState::Sheathed);
            break;
        case SwordState::Held:
            if (!inHand) set(SwordState::Sheathing);
            else if (attacking) set(SwordState::Attacking);
            break;
        case SwordState::Attacking:
            if (!inHand) set(SwordState::Sheathing);
            else if (!attacking) set(SwordState::Held);
            break;
        case SwordState::Sheathing:
            if (inHand) set(SwordState::Held);
            else if (timer > kSheathTime) set(SwordState::Sheathed);
            break;
        }
        return state;
    }
    bool can_swing() const { return state == SwordState::Held || state == SwordState::Attacking; }
    bool hits_allowed() const { return can_swing() && drawnFor > kNoHitAfterDraw; }
    bool sheathed() const { return state == SwordState::Sheathed || state == SwordState::Drawing; }
    void reset() { *this = SwordMachine{}; }

private:
    void set(SwordState s) {
        state = s;
        timer = 0;
    }
};

// The sheath's reach: the controller over the sword hand's shoulder, behind the head (tracking space, metres, in the
// body's frame: x right, y up, z back; side -1 left, +1 right). Inside within `enter` of the point, outside again
// beyond `exit` (no flicker at the edge).
struct ShoulderReach {
    bool inside = false;
    bool update(Vec3 controllerFromHead, int side, float enter = 0.28f, float exit = 0.34f) {
        const Vec3 point{0.15f * float(side), -0.08f, 0.12f};
        const float d = length(controllerFromHead - point);
        inside = inside ? d < exit : d < enter;
        return inside;
    }
};

// ---- items put away after their first-person use (docs/VR_HELD_ITEMS.md, "Put away after use")
// The telescope, the Wind Waker and the Picto Boxes: on the GamePad Link keeps one in his hand after looking through
// it or conducting, until another action puts it away. In first person it is put away (daPy_lk_c::deleteEquipItem)
// once its use ends and he stands or walks again: it stayed in the hand, in the game's logic too (reported)
constexpr uint16_t kItemTelescope = 0x20, kItemWindWaker = 0x22, kItemPictoBox = 0x23, kItemDeluxePictoBox = 0x26;
inline bool put_away_after_use(uint16_t item) {
    return item == kItemTelescope || item == kItemWindWaker || item == kItemPictoBox || item == kItemDeluxePictoBox;
}
// once a step, before Link's logic: `held` his item, `inUse` looking through it or conducting, `idle` first-person
// play with him standing or walking. True once: put it away now. Only after a use of the item held since
struct PutAwayAfterUse {
    uint16_t item = 0;
    bool used = false;
    bool update(uint16_t held, bool inUse, bool idle) {
        if (held != item) {
            item = held;
            used = false;
        }
        if (!put_away_after_use(held)) return false;
        if (inUse) {
            used = true;
            return false;
        }
        if (!used || !idle) return false;
        used = false;
        return true;
    }
};

}  // namespace vr

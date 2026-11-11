// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// The view in a Baba Bud (docs/VR_CONTROLS.md, "Baba Buds"; asked for in the headset, 2026-11-03): in first person,
// Link dropping into a Baba Bud and being shot out of it is shown in third person (the game's camera), and first person
// comes back when he opens the Deku Leaf to glide (its glide pose: the leaf in his hands above his head) or lands.
// Link's actions (decomp d_a_player_main_0x.h, daPyProc_*_e): VOMIT_READY 0x96 (stepping in), VOMIT_WAIT 0x97 (inside),
// VOMIT_JUMP 0x98 (shot out), VOMIT_LAND 0x99 (landing from it), FALL 0x27 (falling after it without the leaf),
// FAN_GLIDE 0x93 (gliding). Pure logic (vr_test checks it); vr_game.cpp applies the switches
#pragma once
#include <cstdint>

namespace vr {

constexpr uint32_t kProcVomitReady = 0x96, kProcVomitWait = 0x97, kProcVomitJump = 0x98, kProcVomitLand = 0x99;
constexpr uint32_t kProcFall = 0x27;
inline bool bud_proc(uint32_t proc) { return proc == kProcVomitReady || proc == kProcVomitWait || proc == kProcVomitJump; }

enum class BudSwitch { None, ToThird, ToFirst };

struct BudView {
    bool active = false;  // the third-person view is the Baba Bud's (switched here, to switch back here)
    // each pad read: Link's action, whether the view is first person now, and whether a switch may happen (normal play
    // with the player's camera, Link valid, not sailing). Returns the switch to make
    BudSwitch step(uint32_t proc, bool firstPerson, bool eligible) {
        if (!active) {
            if (eligible && firstPerson && bud_proc(proc)) {
                active = true;
                return BudSwitch::ToThird;
            }
            return BudSwitch::None;
        }
        // (in the bud, shot out, or falling from it: third person stays; the game's camera is out of play meanwhile,
        // a talk or an event, also waits)
        if (bud_proc(proc) || proc == kProcFall || !eligible) return BudSwitch::None;
        // gliding on the leaf, landing, or anything else: first person again
        active = false;
        return BudSwitch::ToFirst;
    }
    // the player switched the view themself (a right stick click): the bud's switch back is theirs now
    void cancel() { active = false; }
};

}  // namespace vr

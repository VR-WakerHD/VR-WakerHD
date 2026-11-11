// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// VR archery (docs/VR_AIMING.md, "The bow"): the bow in the sword hand, the arrow put on it with the off hand.
//
// The game's bow (daPy_lk_c::setBowReadyAnime, checkNextActionBowReady): taken out, it starts as just after a shot (the
// string relaxed, no arrow); with its item button held it makes an arrow (makeArrow) and draws the string; the button
// let go shoots, and the string relaxes again. So in VR the button is held exactly while the arrow is on the bow:
//   - the off hand brought to the bow (within kNockMetres) with its grip squeezed takes the string and puts the arrow
//     on: the game makes it and draws the string (asked for in the headset: drawn only with the arrow on the bow,
//     relaxed otherwise; taking the string with the grip as another VR game does, asked for with a video);
//   - letting go of that grip once drawn back (the hands kFireDrawMetres apart) shoots it, or either controller's
//     trigger; let go before that, the arrow is dropped: it falls from the bow to the ground (asked for in the
//     headset; the game shoots it, straight down, vr_game.cpp arrow_kept);
//   - the next arrow goes on when the hand is at the bow again (kRenockSeconds after a shot if it stayed there).
// Once on, it stays until shot: the game cannot take an arrow off the string without shooting it.
// Pure (vr_test.cpp); vr_game.cpp feeds it each pad read.
#pragma once
#include "vr_math.h"

namespace vr {

struct BowInput {
    bool aiming = false;                          // the bow aimed (its procedure), first person
    bool mainTrigger = false, offTrigger = false;  // the two controllers' triggers held
    bool stringGrip = false;                       // the arrow hand's grip squeezed (it holds the string)
    float pull = -1.0f;                            // the controllers' own distance (metres; < 0: the hands'), the draw's:
                                                   // Link's shorter, reach-clamped arms made a pull look short
    Vec3 bowHand, stringHand;                      // the bow's hand and the arrow's (metres; the nearer pair of the
                                                   // drawn hands and the controllers)
    float dt = 0;                                  // seconds since the last read
};
struct BowOutput {
    bool onBow = false;   // the arrow on the bow: the game's bow button held (the string drawn)
    bool nocked = false;  // this read: it came on (a pulse in both hands)
    bool fire = false;    // this read: shot
    bool drop = false;    // and let go before drawn back: the arrow dropped, not shot (it falls to the ground)
};

struct BowMachine {
    bool onBow = false;
    bool triggerWas = true;  // (a trigger already held as the bow comes up does not shoot)
    bool gripWas = true;     // (likewise a grip already squeezed as the bow comes up: no release)
    float sinceShot = 1e9f;
    static constexpr float kNockMetres = 0.25f;     // the hands this near put the arrow on the bow
    static constexpr float kRenockSeconds = 0.5f;   // after a shot, the next arrow goes on no sooner (the hand still there)
    // drawn, shot at the grip's release: pulled kMinPullMetres back from where the string was taken, or the hands
    // kFireDrawMetres apart (was 0.32 m apart alone: asked for, easier to shoot)
    static constexpr float kMinPullMetres = 0.08f;
    static constexpr float kFireDrawMetres = 0.25f;
    float nockedAt = 0;  // the hands' distance when the string was taken

    BowOutput update(const BowInput& in) {
        BowOutput out;
        const bool trigger = in.mainTrigger || in.offTrigger;
        if (!in.aiming) {
            *this = BowMachine{};
            return out;
        }
        const bool pressed = trigger && !triggerWas;
        triggerWas = trigger;
        const bool released = gripWas && !in.stringGrip;
        gripWas = in.stringGrip;
        sinceShot += in.dt > 0 && in.dt < 1.0f ? in.dt : 0.0f;
        const Vec3 d = in.bowHand - in.stringHand;
        const bool near = finite(d) && length(d) <= kNockMetres;
        if (!onBow) {
            if (near && in.stringGrip && sinceShot >= kRenockSeconds) {
                onBow = true;
                out.nocked = true;
                nockedAt = in.pull >= 0 ? in.pull : length(d);
            }
        } else if (pressed || released) {
            out.fire = true;
            const float apart = in.pull >= 0 ? in.pull : length(d);
            const bool drawn = finite(d) && std::isfinite(apart) &&
                               (apart >= kFireDrawMetres || apart - nockedAt >= kMinPullMetres);
            out.drop = !pressed && !drawn;
            onBow = false;
            sinceShot = 0;
        }
        out.onBow = onBow;
        return out;
    }
};

// The bow's string in its model (docs/VR_AIMING.md): the two halves of the string meet at its middle, two joints at the
// same place (the Hero's Bow: joints 6 and 11 of 12, equally far from everything in every pose logged). That pair is
// the string, found by its shape: the pair of joints (not the root, 0) within kBowStringSame of each other, the one
// nearest `hand` if there are more. The nearest joint to the drawing hand alone was picked before, and where the game's
// pose holds the hand as far from the string as from the bow's limbs (20 to 60 units from all of them: on the boat,
// after a turn) it took the limbs (joints 1 to 3), and drawing the string pulled the whole bow (reported, 2026-11-06).
// False when the model has no such pair
constexpr float kBowStringSame = 2.0f;  // game units (the logged distances of 6 and 11 differed by a unit at most, rounded)
inline bool bow_string_pair(const Vec3* pos, int n, Vec3 hand, int& a, int& b) {
    a = b = -1;
    float best = 1e30f;
    for (int i = 1; i < n; ++i)
        for (int j = i + 1; j < n; ++j) {
            if (!finite(pos[i]) || !finite(pos[j]) || length(pos[i] - pos[j]) > kBowStringSame) continue;
            const float d = finite(hand) ? length(pos[i] - hand) : 0.0f;
            if (d < best) best = d, a = j, b = i;
        }
    return a > 0;
}

}  // namespace vr

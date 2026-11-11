// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// The Forsaken Fortress VR test environment (docs/VR_TEST_ENVIRONMENT.md): a repeatable place with every item, for
// validating the VR interactions one test at a time (docs/VR_TEST_REGISTRY.md).
//
// Entered with --vr-test (or WWHD_VR_TEST_FORTRESS=1) once a save is in play, or from F1 > VR > Test environment.
// Built from the game's own services, nothing of its own:
//   - isolation: the save folder is copied and in-game saves go to the copy (hle_fs::set_save_root, as the world
//     editor's test mode does), and a portable state of the world as it was is written first; leaving loads it back,
//     so the player's own progress, inventory and place return exactly as they were;
//   - the place: the game's own stage change to Forsaken Fortress (MajyuE room 0, a spawn point of the stage's own
//     PLYR list);
//   - the loadout: the Gameplay cheats' item, stat, bag and sword edits (mods/cheats.cpp, which give each item as the
//     game's item_func_* do), bottles with contents, the Picto Box variant chosen, and while active the infinite
//     health, magic and ammo switches (each switch put back as it was on leaving);
//   - reset: a portable state written on arrival, loaded again (then the loadout again).
// The stage machine below is pure (vr_test.cpp); vr_test_env.cpp does what it asks with the game's services.
#pragma once
#include <cstdint>
#include <string>

namespace vr::test_env {

// ---- the place
constexpr const char* kStage = "MajyuE";  // Forsaken Fortress, outside
constexpr int kRoom = 0;
// the stage's spawn point (MajyuE_Room0 PLYR): point 0 at (-230, 37, 305), the courtyard's lower level by the
// entrance, room to swing and aim with walls, crates, barrels, ramps and the fortress's enemies about
constexpr int kPoint = 0;

// ---- the stage machine
enum class Phase : uint8_t {
    Off,            // normal play
    SavingReturn,   // writing the return state (the world as it was)
    Warping,        // the stage change asked for
    Arriving,       // in the stage: waiting until Link is under control
    Loadout,        // the items and resources being given
    SavingFixture,  // writing the reset state
    Ready,          // testing
    Resetting,      // the reset state being loaded
    Leaving,        // the return state being loaded
};
inline const char* phase_name(Phase p) {
    switch (p) {
    case Phase::Off: return "off";
    case Phase::SavingReturn: return "saving the world to return to";
    case Phase::Warping: return "going to Forsaken Fortress";
    case Phase::Arriving: return "arriving";
    case Phase::Loadout: return "giving the items";
    case Phase::SavingFixture: return "saving the reset state";
    case Phase::Ready: return "ready";
    case Phase::Resetting: return "resetting";
    case Phase::Leaving: return "leaving";
    }
    return "?";
}

// what the game is doing now (vr_test_env.cpp reads it each frame)
struct Facts {
    bool saveInPlay = false;  // a save file is loaded and played (not the title or file select)
    bool controllable = false;  // Link under the player's control (no event, message, menu)
    bool inStage = false;  // Link is in kStage, kRoom
};
// events since the last update (the services' answers)
struct Events {
    bool enter = false, reset = false, refill = false, exit = false;  // asked for
    bool done = false, failed = false;  // the pending service finished / was refused
};
enum class Action : uint8_t {
    None,
    Isolate,       // copy the saves, redirect in-game saves, write the return state
    Warp,          // the stage change to the place
    GiveLoadout,   // the items and resources
    SaveFixture,   // write the reset state
    LoadFixture,   // load the reset state
    Restore,       // load the return state, saves back to the player's own
    Abandon,       // entering failed: saves back to the player's own, nothing loaded
};
struct Step {
    Action action = Action::None;
    Phase phase = Phase::Off;
    const char* note = nullptr;  // a line for the log and the status
};

struct Machine {
    Phase phase = Phase::Off;
    int steady = 0;  // frames Link has been in the stage and under control (Arriving)
    bool wantEnter = false;  // asked for before a save was in play: entered when one is
    static constexpr int kSettleFrames = 30;   // in control this long before the loadout (the stage's own start)
    static constexpr int kLoadoutFrames = 4;   // the cheats apply at the next frame; given after this many
    static constexpr int kChangeFrames = 20;   // after a stage change starts or a state loads: not counted yet
    int timer = 0;
    int tries = 0;  // attempts to save the return state
    int fixtureTries = 0;  // attempts to save the reset state
    static constexpr int kTries = 5;

    Step update(const Facts& f, const Events& e) {
        Step s;
        if (e.enter && phase == Phase::Off) {
            wantEnter = true;
            tries = 0;
        }
        auto go = [&](Phase p, Action a, const char* note) {
            phase = p;
            s.action = a;
            s.note = note;
            timer = 0;
        };
        ++timer;
        switch (phase) {
        case Phase::Off:
            // in control for a while first (a save just loaded, a scene change ending: the state cannot be saved)
            steady = wantEnter && f.saveInPlay && f.controllable ? steady + 1 : 0;
            if (steady >= kSettleFrames) {
                wantEnter = false;
                steady = 0;
                go(Phase::SavingReturn, Action::Isolate, "entering: saves isolated, the world as it was saved to return to");
            }
            break;
        case Phase::SavingReturn:
            if (e.failed) {
                // tried again a little later, a few times (the game was busy for a moment)
                const bool again = ++tries < kTries;
                wantEnter = again;
                go(Phase::Off, Action::Abandon, again ? "the world could not be saved yet: trying again" : "could not save the world to return to: not entered");
            } else if (e.done) {
                tries = 0;
                go(Phase::Warping, Action::Warp, "to Forsaken Fortress");
            }
            break;
        case Phase::Warping:  // done: the stage change started (savestate.cpp service_warp)
            if (e.failed) go(Phase::Leaving, Action::Restore, "the stage change was refused: back");
            else if (e.done) { phase = Phase::Arriving; steady = 0; timer = 0; }
            break;
        case Phase::Resetting:  // done: the reset state loaded
            if (e.failed) go(Phase::Warping, Action::Warp, "the reset state could not be loaded: to Forsaken Fortress");
            else if (e.done) { phase = Phase::Arriving; steady = 0; timer = 0; }
            break;
        case Phase::Arriving:
            // (the old stage is still shown for a few frames after the change starts: the wait counts from then)
            steady = f.inStage && f.controllable && timer > kChangeFrames ? steady + 1 : 0;
            if (steady >= kSettleFrames) go(Phase::Loadout, Action::GiveLoadout, "giving the items");
            break;
        case Phase::Loadout:
            if (timer >= kLoadoutFrames) {
                if (!fixtureSaved) go(Phase::SavingFixture, Action::SaveFixture, "saving the reset state");
                else go(Phase::Ready, Action::None, "ready");
            }
            break;
        case Phase::SavingFixture:
            if (e.failed && ++fixtureTries < kTries) {
                // the stage's own start still running (seen in the headset: a scene transition): tried again once
                // Link has been in control a while (the loadout given again on the way, harmless)
                phase = Phase::Arriving;
                steady = 0;
                timer = 0;
                s.note = "the reset state could not be saved yet: trying again";
            } else if (e.done || e.failed) {
                fixtureSaved = e.done;
                fixtureTries = 0;
                go(Phase::Ready, Action::None, e.done ? "ready" : "ready (no reset state: reset warps again)");
            }
            break;
        case Phase::Ready:
            if (e.exit) go(Phase::Leaving, Action::Restore, "leaving: the world as it was");
            else if (e.reset) {
                if (fixtureSaved) go(Phase::Resetting, Action::LoadFixture, "resetting");
                else go(Phase::Warping, Action::Warp, "resetting: to Forsaken Fortress");
                steady = 0;
            } else if (e.refill) go(Phase::Loadout, Action::GiveLoadout, "refilling");
            break;
        case Phase::Leaving:
            if (e.done || e.failed) {
                fixtureSaved = false;
                go(Phase::Off, Action::None, e.done ? "left: the world as it was" : "left (the world could not be loaded back)");
            }
            break;
        }
        s.phase = phase;
        return s;
    }
    bool fixtureSaved = false;
};

// ---- the game side (vr_test_env.cpp)
void set_launch_request();  // main(): --vr-test
bool active();               // not Off (any thread)
Phase phase();               // any thread
std::string status();        // the last note (any thread)
void request_enter();
void request_reset();
void request_refill();
void request_exit();
// the loadout's choices (used at the next loadout; refill to apply now)
void set_deluxe_picto(bool deluxe);
bool deluxe_picto();
void set_unlimited(bool on);  // infinite health, magic and ammo while active (default on)
bool unlimited();
void service();  // game main thread, frame start (interp.cpp, after the cheats)

}  // namespace vr::test_env

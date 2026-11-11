// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// VR controller bindings (docs/VR_CONTROLS.md): OpenXR actions -> the Wii U GamePad buttons the game reads.
//
// The OpenXR side is generic (openxr_session.cpp suggests bindings for the Khronos simple controller and
// the common motion-controller profiles; the runtime remaps them for other controllers). Which GamePad
// button each action presses is a setting ("vr.bind.<action>" = an input_map button name such as "ZL"),
// so no headset's layout is built in. Hands are "main" (the sword hand) and "off" (the shield hand):
// left-handed mode swaps which physical controller is which.
#pragma once
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace vr {

// VPAD button bits (input.h; repeated so this header stays free of the host input layer)
namespace pad {
enum : uint32_t {
    kA = 0x8000, kB = 0x4000, kX = 0x2000, kY = 0x1000,
    kL = 0x0020, kR = 0x0010, kZL = 0x0080, kZR = 0x0040,
    kPlus = 0x0008, kMinus = 0x0004, kHome = 0x0002,
    kUp = 0x0200, kDown = 0x0100, kLeft = 0x0800, kRight = 0x0400,
    kStickR = 0x00020000, kStickL = 0x00040000,
};
}

enum class Action : int {
    MainTrigger, MainGrip, MainPrimary, MainSecondary, MainStickClick,
    OffTrigger, OffGrip, OffPrimary, OffSecondary, OffStickClick,
    Menu, RightStickDown, Count
};
constexpr int kActionCount = int(Action::Count);

struct ActionInfo {
    const char* id;      // the setting's name: vr.bind.<id>
    const char* label;
    uint32_t button;     // default GamePad button
};
inline const ActionInfo kActions[kActionCount] = {
    {"mainTrigger", "Sword hand trigger", 0},              // nothing: it opens the item wheel while Link holds nothing
                                                            // (A is the primary button); aimed items fire with it
    {"mainGrip", "Sword hand grip", pad::kR},              // item on R
    {"mainPrimary", "Sword hand primary button", pad::kA},
    {"mainSecondary", "Sword hand secondary button", pad::kB},  // sword button (swings work without it)
    {"mainStickClick", "Sword hand stick click", pad::kMinus},
    {"offTrigger", "Shield hand trigger", pad::kZL},       // target (lock-on)
    {"offGrip", "Shield hand grip", 0},                     // nothing: the grips close Link's hands (crouch:
                                                            // the right stick down)
    {"offPrimary", "Shield hand primary button", pad::kX},
    {"offSecondary", "Shield hand secondary button", pad::kY},
    {"offStickClick", "Shield hand stick click", pad::kStickL},
    {"menu", "Menu button", pad::kPlus},
    {"rightStickDown", "Right stick down", pad::kZR},      // crouch (held: Link crouches while it is held)
};


struct ButtonName {
    const char* name;
    uint32_t bit;
};
// the names input_map.cpp uses for the GamePad buttons ("" = nothing)
inline const ButtonName kButtonNames[] = {
    {"A", pad::kA}, {"B", pad::kB}, {"X", pad::kX}, {"Y", pad::kY}, {"L", pad::kL}, {"R", pad::kR},
    {"ZL", pad::kZL}, {"ZR", pad::kZR}, {"Plus", pad::kPlus}, {"Minus", pad::kMinus}, {"Home", pad::kHome},
    {"Up", pad::kUp}, {"Down", pad::kDown}, {"Left", pad::kLeft}, {"Right", pad::kRight},
    {"LeftStickClick", pad::kStickL}, {"RightStickClick", pad::kStickR}, {"", 0},
};
inline bool button_from_name(const std::string& n, uint32_t& bit) {
    for (const auto& b : kButtonNames)
        if (n == b.name) { bit = b.bit; return true; }
    return false;
}
inline const char* button_name(uint32_t bit) {
    for (const auto& b : kButtonNames)
        if (b.bit == bit) return b.name;
    return "";
}

struct Bindings {
    uint32_t button[kActionCount];
    Bindings() { for (int i = 0; i < kActionCount; ++i) button[i] = kActions[i].button; }
    // vr.bind.<id> = <button name>; false for another key or an unknown button (the binding stays)
    bool from_kv(const std::string& key, const std::string& value) {
        static const std::string prefix = "vr.bind.";
        if (key.compare(0, prefix.size(), prefix) != 0) return false;
        const std::string id = key.substr(prefix.size());
        for (int i = 0; i < kActionCount; ++i)
            if (id == kActions[i].id) {
                uint32_t bit;
                if (!button_from_name(value, bit)) return false;
                button[i] = bit;
                return true;
            }
        return false;
    }
    std::vector<std::pair<std::string, std::string>> to_kv() const {
        std::vector<std::pair<std::string, std::string>> out;
        for (int i = 0; i < kActionCount; ++i)
            out.emplace_back(std::string("vr.bind.") + kActions[i].id, button_name(button[i]));
        return out;
    }
    // the GamePad buttons held for a set of held actions (bit i = Action i)
    uint32_t buttons(uint32_t heldActions) const {
        uint32_t b = 0;
        for (int i = 0; i < kActionCount; ++i)
            if (heldActions & (1u << i)) b |= button[i];
        return b;
    }
};

// Settings saved before format 2 had the shield-hand grip on ZR (crouch); the right stick down crouches now and
// the grips close Link's hands, so that saved default is cleared once (a binding chosen since stays)
// Before format 10 the sword hand's trigger pressed A; it opens the item wheel now while Link holds nothing (asked for
// in the headset), and A stays on the primary button, so that saved default is cleared once too
inline void migrate_bindings(Bindings& b, int savedFormat) {
    if (savedFormat < 2 && b.button[int(Action::OffGrip)] == pad::kZR) b.button[int(Action::OffGrip)] = 0;
    if (savedFormat < 10 && b.button[int(Action::MainTrigger)] == pad::kA) b.button[int(Action::MainTrigger)] = 0;
}

// A thumbstick pushed down, with hysteresis: pressed below -press (OpenXR's y is +1 up, -1 down) while it points
// more down than sideways, released above -release; up, left, right and drift around the centre never press it
struct StickDown {
    bool held = false;
    bool update(float x, float y, float press = 0.6f, float release = 0.4f) {
        if (!held) held = y < -press && std::fabs(x) < -y;
        else held = y < -release;
        return held;
    }
};
// A grip closing a hand, with hysteresis: closed above `close`, open again below `open`
struct GripLatch {
    bool closed = false;
    bool update(float value, float close = 0.6f, float open = 0.45f) {
        closed = closed ? value > open : value > close;
        return closed;
    }
};

// The camera view toggle (docs/VR_CONTROLS.md): a short click of the right stick, counted when it is released, and
// only if the left stick was not clicked during it (both clicks held are the recentre gesture below) and it was
// shorter than `maxHold` (a held click never toggles, repeatedly or at all)
class ClickToggle {
public:
    bool update(bool right, bool left, float dt, float maxHold = 0.5f) {
        bool fire = false;
        if (right) {
            if (!down_) { down_ = true; held_ = 0; spoiled_ = false; }
            held_ += dt;
            spoiled_ = spoiled_ || left || held_ > maxHold;
        } else if (down_) {
            fire = !spoiled_;
            down_ = false;
        }
        return fire;
    }
    void reset() { down_ = false; spoiled_ = true; }
private:
    bool down_ = false, spoiled_ = false;
    float held_ = 0;
};

// Recentre: both stick clicks held together for kRecenterHold seconds (a deliberate combination that no
// single game button uses)
constexpr float kRecenterHold = 0.6f;
class RecenterGesture {
public:
    // true once per hold, when the combination has been held long enough
    bool update(bool mainClick, bool offClick, float dt) {
        if (!(mainClick && offClick)) { held_ = 0; fired_ = false; return false; }
        held_ += dt;
        if (!fired_ && held_ >= kRecenterHold) { fired_ = true; return true; }
        return false;
    }
    bool holding() const { return held_ > 0; }

private:
    float held_ = 0;
    bool fired_ = false;
};

}  // namespace vr

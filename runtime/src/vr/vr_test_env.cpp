// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// The Forsaken Fortress VR test environment: see vr_test_env.h and docs/VR_TEST_ENVIRONMENT.md.
#include "vr_test_env.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "guest_addr.h"
#include "hle/save_root.h"
#include "mods/mods.h"
#include "runtime.h"
#include "savestate.h"

namespace vr::test_env {
namespace fs = std::filesystem;
namespace {

// the save (dSv_info_c's player, as vr_game.cpp reads it) and the play's copies of the buttons and equipment
const uint32_t kSavePtr = GD(0x101F84DC);
const uint32_t kPlay = GD(0x1046F0B0);
constexpr uint32_t kSaveMaxLife = 0x20, kSaveLife = 0x22, kSaveSelectKeys = 0x29, kSaveSelectEquip = 0x2E,
                   kSaveItems = 0x5C, kSaveGot = 0x71;
constexpr uint32_t kPlaySelectItems = 0x5BBB, kPlaySelectEquip = 0x5BC0;  // mSelectItem[5], mSelectEquip[5]
constexpr uint8_t kNoItem = 0xFF;
// the inventory's slots this sets beyond the cheats' "items" (mods/cheats.cpp all_items): the Picto Box's variant
// (slot 8) and the four bottles with contents, so a full bottle, a fairy and water can be used as well as an empty one
constexpr int kPictoSlot = 8, kBottleSlots = 14;
constexpr uint8_t kPicto = 0x23, kDeluxePicto = 0x26;
constexpr uint8_t kBottles[4] = {0x50, 0x51, 0x57, 0x56};  // empty, red potion, fairy, water
// the sword and shield the loadout equips (the cheats' "sword": the Master Sword at full power, the Mirror Shield)
constexpr uint8_t kMasterSword3 = 0x3E, kMirrorShield = 0x3C;

std::mutex g_mu;
Machine g_machine;          // game thread (under g_mu for the any-thread reads)
Events g_asked;             // any thread asks (under g_mu)
std::atomic<int> g_answer{0};  // the pending service's answer: 1 done, -1 failed (its callback, any thread)
std::string g_status = "off";
std::atomic<Phase> g_phase{Phase::Off};
std::atomic<bool> g_deluxe{true}, g_unlimited{true};
int g_infiniteBefore = 0;   // the infinite switches as they were on entering
bool g_launch = [] { const char* e = std::getenv("WWHD_VR_TEST_FORTRESS"); return e && *e == '1'; }();

fs::path dir() { return fs::path(ss::states_dir()) / "vr-test"; }
fs::path return_state() { return dir() / "return.wwstate"; }
fs::path fixture_state() { return dir() / "fortress.wwstate"; }

uint32_t save_base() {
    const uint32_t b = ld32(kSavePtr);
    if (!b) return 0;
    const uint16_t max = ld16(b + kSaveMaxLife), life = ld16(b + kSaveLife);
    return max >= 12 && max <= 80 && life <= max ? b : 0;
}

void answer(bool ok, const std::string& why) {
    if (!ok) LOG("[vr test] %s", why.c_str());
    g_answer = ok ? 1 : -1;
}

void set_status(const char* note) {
    if (!note) return;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_status = note;
    }
    LOG("[vr test] %s", note);
    ss::notice(std::string("VR test: ") + note);
}

// the Picto Box's variant in its slot, and on the button that holds it
void give_picto(uint32_t s) {
    const uint8_t want = g_deluxe ? kDeluxePicto : kPicto;
    const uint8_t was = ld8(s + kSaveItems + kPictoSlot);
    st8(s + kSaveItems + kPictoSlot, want);
    st8(s + kSaveGot + kPictoSlot, ld8(s + kSaveGot + kPictoSlot) | (g_deluxe ? 3 : 1));
    for (int b = 0; b < 3; ++b)
        if (ld8(s + kSaveSelectKeys + b) == kPictoSlot && (was == kPicto || was == kDeluxePicto))
            st8(kPlay + kPlaySelectItems + b, want);
}

// after the cheats of GiveLoadout applied (a few frames later): what they leave as it was
void finish_loadout() {
    const uint32_t s = save_base();
    if (!s) return;
    give_picto(s);
    for (int i = 0; i < 4; ++i) {
        st8(s + kSaveItems + kBottleSlots + i, kBottles[i]);
        for (int b = 0; b < 3; ++b)
            if (ld8(s + kSaveSelectKeys + b) == kBottleSlots + i) st8(kPlay + kPlaySelectItems + b, kBottles[i]);
    }
    // the sword and shield in the play's copy too: the fortress's first visit takes the sword off both at its
    // load (d_s_play: dStageType_FF1 with event 0x0801), the cheat sets the save's
    st8(kPlay + kPlaySelectEquip + 0, kMasterSword3);
    st8(kPlay + kPlaySelectEquip + 1, kMirrorShield);
    st8(s + kSaveSelectEquip + 0, kMasterSword3);
    st8(s + kSaveSelectEquip + 1, kMirrorShield);
}

void do_action(const Step& st) {
    std::string why;
    switch (st.action) {
    case Action::None:
        break;
    case Action::Isolate: {
        // a copy of the player's saves; in-game saves go to it until leaving
        std::error_code ec;
        fs::remove_all(dir() / "save", ec);
        fs::create_directories(dir() / "save", ec);
        fs::copy(config::save_dir, dir() / "save", fs::copy_options::recursive, ec);
        if (ec) {
            answer(false, "cannot copy the save folder: " + ec.message());
            break;
        }
        ss::set_auto_save_paused(true);
        hle_fs::set_save_root((dir() / "save").string());
        g_answer = 0;
        ss::request_save_portable_file(return_state().string(), answer);
        break;
    }
    case Action::Warp: {
        ss::Warp w;
        w.stage = kStage;
        w.room = kRoom;
        w.point = kPoint;
        g_answer = 0;
        ss::request_warp(w, answer);
        break;
    }
    case Action::GiveLoadout:
        mods::request_cheat(mods::kCheatItems | mods::kCheatStats | mods::kCheatBags | mods::kCheatSword);
        if (g_unlimited) mods::set_infinite(mods::kInfHealth | mods::kInfMagic | mods::kInfAmmo, true);
        break;
    case Action::SaveFixture:
        g_answer = 0;
        ss::request_save_portable_file(fixture_state().string(), answer);
        break;
    case Action::LoadFixture:
        g_answer = 0;
        ss::request_load_portable_file(fixture_state().string(), answer);
        break;
    case Action::Restore: {
        // the infinite switches as they were, the player's saves, then the world as it was
        const int all = mods::kInfHealth | mods::kInfMagic | mods::kInfAmmo;
        mods::set_infinite(all, false);
        mods::set_infinite(g_infiniteBefore & all, true);
        hle_fs::set_save_root("");
        ss::set_auto_save_paused(false);
        g_answer = 0;
        ss::request_load_portable_file(return_state().string(), answer);
        break;
    }
    case Action::Abandon:
        hle_fs::set_save_root("");
        ss::set_auto_save_paused(false);
        break;
    }
}

}  // namespace

void set_launch_request() { g_launch = true; }
bool active() { return g_phase.load() != Phase::Off; }
Phase phase() { return g_phase.load(); }
std::string status() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_status;
}
void request_enter() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_asked.enter = true;
}
void request_reset() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_asked.reset = true;
}
void request_refill() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_asked.refill = true;
}
void request_exit() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_asked.exit = true;
}
void set_deluxe_picto(bool deluxe) { g_deluxe = deluxe; }
bool deluxe_picto() { return g_deluxe; }
void set_unlimited(bool on) {
    g_unlimited = on;
    if (active()) mods::set_infinite(mods::kInfHealth | mods::kInfMagic | mods::kInfAmmo, on);
}
bool unlimited() { return g_unlimited; }

// test aid: WWHD_VR_TEST_ENV_AT=frame:what[,frame:what...] (what: enter, reset, refill, exit) asks at those frames of
// this service (one a frame), as the F1 buttons do
void test_requests() {
    struct At {
        unsigned long long frame;
        char what[8];
    };
    static const std::vector<At> list = [] {
        std::vector<At> out;
        const char* e = std::getenv("WWHD_VR_TEST_ENV_AT");
        while (e && *e) {
            At a{};
            if (std::sscanf(e, "%llu:%7[a-z]", &a.frame, a.what) == 2) out.push_back(a);
            e = std::strchr(e, ',');
            if (e) ++e;
        }
        return out;
    }();
    static unsigned long long frame = 0;
    static size_t next = 0;
    ++frame;
    while (next < list.size() && frame >= list[next].frame) {
        const std::string what = list[next++].what;
        LOG("[vr test] test aid: %s at frame %llu", what.c_str(), frame);
        if (what == "enter") request_enter();
        else if (what == "reset") request_reset();
        else if (what == "refill") request_refill();
        else if (what == "exit") request_exit();
    }
}

void service() {
    test_requests();
    Events e;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        e = g_asked;
        g_asked = {};
    }
    if (g_launch) {  // --vr-test: entered as soon as a save is in play
        g_launch = false;
        e.enter = true;
        LOG("[vr test] started with --vr-test: entering once a save is in play");
    }
    const int a = g_answer.exchange(0);
    e.done = a > 0;
    e.failed = a < 0;
    Facts f;
    const std::string problem = ss::control_problem();
    f.saveInPlay = problem != "no game in progress" && save_base() != 0;
    f.controllable = problem.empty();
    const ss::LinkPlace place = ss::link_place();
    f.inStage = place.valid && place.stage == kStage && place.room == kRoom;
    const Phase before = g_machine.phase;
    const Step st = g_machine.update(f, e);
    if (st.action == Action::Isolate)  // the infinite switches as the player had them, put back on leaving
        g_infiniteBefore = (mods::infinite(mods::kInfHealth) ? mods::kInfHealth : 0) |
                           (mods::infinite(mods::kInfMagic) ? mods::kInfMagic : 0) |
                           (mods::infinite(mods::kInfAmmo) ? mods::kInfAmmo : 0);
    if (before == Phase::Loadout && g_machine.phase != Phase::Loadout) finish_loadout();
    // the game closed while testing: its "Last session" is the player's world from before, not this one (seen in the
    // headset: quitting in the fortress left the test world to resume)
    if (before == Phase::SavingReturn && st.action == Action::Warp) ss::set_last_session_source(return_state().string());
    if (st.action == Action::Restore || st.action == Action::Abandon) ss::set_last_session_source("");
    g_phase = g_machine.phase;
    set_status(st.note);
    do_action(st);
}

}  // namespace vr::test_env

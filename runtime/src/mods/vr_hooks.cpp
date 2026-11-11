// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// VR mode's game hooks that need a function of their own (tools/recomp/hooks_vr.txt; vr/vr_game.h).
// Defined in every build: the recompiled code calls them whether or not VR is built in.
#include "runtime.h"
#include "guest_addr.h"
#include "interp.h"
#ifdef WWHD_VR
#include "vr/vr_game.h"
#include "vr/vr_ui.h"
#include "gfx/stereo.h"
#include "gx2/gx2_cmd.h"
#endif

extern "C" {
void f_023DBBE4_orig(Cpu* c);  // daPy_Draw
void f_024F8018_orig(Cpu* c);  // dCam_getControledAngleY
void f_02403150_orig(Cpu* c);  // daPy_lk_c::jointCB1
void f_020C0E04_orig(Cpu* c);  // daBoko_Draw
void f_025A83AC_orig(Cpu* c);  // dPa_control_c::draw
void f_02506964_orig(Cpu* c);  // dCamera_c::CalcSubjectAngle
void f_02414834_orig(Cpu* c);  // daPy_sightPacket_c::draw
void f_023D8624_orig(Cpu* c);  // daPy_lk_c::setDrawHandModel
void f_0205244C_orig(Cpu* c);  // daArrow_c::setKeepMatrix
void f_020CDBFC_orig(Cpu* c);  // daBoomerang_sightPacket_c::draw
void f_0242FA80_orig(Cpu* c);  // daPy_lk_c::procBtJumpCut_init
void f_02430078_orig(Cpu* c);  // daPy_lk_c::procBtRollCut_init
void f_024306C8_orig(Cpu* c);  // daPy_lk_c::procBtVerticalJumpCut_init
void f_023EC370_orig(Cpu* c);  // daPy_lk_c::setSpecialBattle
void f_023E5400_orig(Cpu* c);  // daPy_lk_c::changeSpecialBattle
void f_026FD1DC_orig(Cpu* c);  // hdMsgWindow_updateWaitInput (the message window waiting for the button)
void f_026FCC10_orig(Cpu* c);  // hdMsgWindow_updateOpen (the message window typing)
void f_026FCEE0_orig(Cpu* c);  // hdMsgWindow_updateStop (a page's stop waiting for A)
}

// dCamera_c::CalcSubjectAngle(this, s16* pitch, s16* yaw) (02506964): the first-person camera's aim from the sticks (and
// the GamePad's gyro); in VR an aimed item (bow, hookshot, boomerang, grappling hook) is aimed where the sword hand's
// controller points (docs/VR_AIMING.md). The game's own result is kept for everything else
// daPy_lk_c::setDrawHandModel (023D8624): Link's left hand kept drawn while the bow is aimed with its string relaxed.
// The HD game hides that hand (its material's flags & ~6) in the first-person bow aim (procedure 0x94, 0x8D on the boat) during the
// shoot animation, the bow's pose before an arrow is on it: in VR it is the player's hand (seen in the headset: the left
// hand invisible before the arrow was drawn). The procedure reads as another while the hand models are chosen (the
// function's only other reads of it are the demo tool's and the guard crash's), then is put back
extern "C" void hook_023D8624(Cpu* c) {
#ifdef WWHD_VR
    const uint32_t link = c->r[3];
    constexpr uint32_t kCurProc = 0x65F0;  // daPy_lk_c::mCurProc
    if (link && vr::game::keep_bow_hand(link)) {
        const uint32_t proc = ld32(link + kCurProc);
        st32(link + kCurProc, 0xFFFF);
        f_023D8624_orig(c);
        st32(link + kCurProc, proc);
        return;
    }
#endif
    f_023D8624_orig(c);
}

// daPy_sightPacket_c::draw (02414834): the aiming sight drawn smaller in VR. Its quad is placed by the packet's matrix
// (+0x14, built once a step by setSight); scaled about its centre for this draw only and put back, so the frames that
// draw it again do not shrink it further.
// It is a 2D mark as the boomerang's lock-on sights are (setSight projects mPos to the screen with the game's own camera
// and puts it on the 2D draw list), so in the eye pictures each eye drew it at the same pixel of a different field and
// it showed twice: the hookshot's, the bow's and the boomerang's aim mark in third person, on land and on the boat
// (reported, 2026-11-06: "double red dot sight"). In VR it is drawn once into the UI layer's image, as the boomerang's
// lock-on sights are (hook_020CDBFC)
extern "C" void hook_02414834(Cpu* c) {
#ifdef WWHD_VR
    const uint32_t packet = c->r[3];
    const float k = vr::game::sight_scale();
    const bool uiLayer = stereo::requestedXR.load(std::memory_order_relaxed);
    if (uiLayer) gx2::emit(gx2::OP_LAYOUT_CONTENT, {0u, 1u, 0u});  // (begin: no content clip, a layout, not a photograph)
    if (packet && k != 1.0f) {
        constexpr uint32_t kSightMtx = 0x14;
        uint32_t saved[12];
        for (int i = 0; i < 12; ++i) saved[i] = ld32(packet + kSightMtx + i * 4);
        for (int r = 0; r < 3; ++r)
            for (int col = 0; col < 3; ++col) {
                const uint32_t a = packet + kSightMtx + (r * 4 + col) * 4;
                stf32(a, float(ldf32(a)) * k);
            }
        f_02414834_orig(c);
        for (int i = 0; i < 12; ++i) st32(packet + kSightMtx + i * 4, saved[i]);
    } else {
        f_02414834_orig(c);
    }
    if (uiLayer) gx2::emit(gx2::OP_LAYOUT_CONTENT, {0u, 0u});  // (end)
    return;
#endif
    f_02414834_orig(c);
}

// the parry's window (setSpecialBattle 023EC370: the prompt; changeSpecialBattle 023E5400: the parry's start): in VR
// first person the locked-on enemy's window widened for the call and put back after it (vr::game::parry_widen;
// docs/VR_CONTROLS.md, "Parry attacks"). changeSpecialBattle's result (r3) is the game's
static void with_parry_widened(Cpu* c, void (*fn)(Cpu*)) {
#ifdef WWHD_VR
    uint32_t saved[3];
    const uint32_t enemy = vr::game::parry_widen(c->r[3], saved);
    fn(c);
    if (enemy) vr::game::parry_unwiden(enemy, saved);
#else
    fn(c);
#endif
}
extern "C" void hook_023EC370(Cpu* c) { with_parry_widened(c, f_023EC370_orig); }
extern "C" void hook_023E5400(Cpu* c) { with_parry_widened(c, f_023E5400_orig); }

// A conversation's page waiting for the button (hdMsgWindow_updateWaitInput 026FD1DC, decomp d/hd_msg_window_base.cpp):
// in VR the white that the text turns as it is spoken stopped at a colour change ("aux <red>fleurs bombes</red>", the
// French Dragon Roost page, a Quest 3 player through Virtual Desktop, 2026-11-05: the page's arrow up, the rest faint until
// the next page; the same page had come out whole the first time it was shown). At the end of a page (the next stop
// offset negative: only the button is left) its letters are all shown, as the game shows them when a button skips the
// typing (mCharacterCount 0x14F). Logged with the window's count and stops when it was short, for the cause
#ifdef WWHD_VR
#include "vr/vr.h"
#endif
#ifdef WWHD_VR
// The VR text speed (WWHD_VR_TEXT_SPEED, 1..8): 1, the game's own, by default since 2026-11-09 (asked for: "lets make
// everything back to regular defaults for text"); 4 before. Above 1 the message windows type that much faster
// (hook_026FCC10) and a page's end shows all its letters (hook_026FD1DC)
// A page at its stop (hdMsgWindow_updateStop 026FCEE0, hdMsgWindow_updateWaitInput 026FD1DC: the page's end or a pause,
// waiting for A). While it waits the game updates its text each step (windowUpdatePane), and that is where the last
// letters turn white; an A in the first steps there closed the page with letters still dark (asked for, 2026-11-09:
// "not all text gets highlighted white", "the text is forwarding too fast"). In VR the first kStopSettle steps at a
// stop take no A (the window's confirm and skip flags cleared for that call); the page then closes on A as before
struct StopSeen { uint32_t window = 0, message = ~0u, stop = ~0u; uint64_t since = 0; };
static constexpr uint64_t kStopSettle = 6;  // steps (0.2 s)
static bool stop_settling(uint32_t s) {
    constexpr uint32_t kMessageId = 0x5C, kStopIndex = 0xE8;
    static StopSeen seen[4];
    const uint32_t message = ld32(s + kMessageId), stop = ld32(s + kStopIndex);
    const uint64_t now = interp::logic_steps();
    StopSeen* w = nullptr;
    for (auto& e : seen)
        if (e.window == s) w = &e;
    if (!w) {
        static int next = 0;
        w = &seen[next++ & 3];
        w->window = 0;
    }
    if (w->window != s || w->message != message || w->stop != stop || now < w->since || now > w->since + 600) {
        *w = {s, message, stop, now};  // (a new stop: or one left long ago and come back to)
    }
    return now < w->since + kStopSettle;
}
// clears a window's A (confirm) and skip input for this call while its stop settles; logged a few times
static void hold_input_while_settling(uint32_t s, const char* where) {
    constexpr uint32_t kInputFlags = 0x54, kConfirm = 1, kSkip = 2;
    if (!stop_settling(s)) return;
    const uint32_t flags = ld32(s + kInputFlags);
    if (!(flags & (kConfirm | kSkip))) return;
    st32(s + kInputFlags, flags & ~(kConfirm | kSkip));
    static int logged = 0;
    if (logged++ < 40)
        LOG("[vr] message window %08X: A at a %s's first steps held (the page still turning white; message %u, stop %u)",
            s, where, ld32(s + 0x5C), ld32(s + 0xE8));
}
static int vr_text_speed() {
    static const int speed = [] {
        const char* e = std::getenv("WWHD_VR_TEXT_SPEED");
        const int v = e && *e ? std::atoi(e) : 1;
        return v >= 1 && v <= 8 ? v : 1;
    }();
    return speed;
}
#endif
extern "C" void hook_026FD1DC(Cpu* c) {
#ifdef WWHD_VR
    constexpr uint32_t kCharacterCount = 0x90, kStopOffsets = 0x98, kStopCounts = 0xC0, kStopIndex = 0xE8;
    constexpr int32_t kAllShown = 0x14F;
    const uint32_t s = c->r[3];
    if (vr::active() && s) hold_input_while_settling(s, "page end");
    if (vr::active() && s) {
        const uint32_t next = ld32(s + kStopIndex) + 1;
        const int32_t nextOffset = int32_t(ld32(s + kStopOffsets + 4 * (next < 10 ? next : 0)));
        const int32_t count = int32_t(ld32(s + kCharacterCount));
        {
            // (diagnostics, 2026-11-09: a page left black, not turned white, with no "all shown" line) each page that
            // waits for the button, once: its window, message, letters shown and stops
            static uint32_t lastWindow = 0, lastMessage = ~0u, lastStop = ~0u, pages = 0;
            const uint32_t message = ld32(s + 0x5C), stop = ld32(s + kStopIndex);
            if ((s != lastWindow || message != lastMessage || stop != lastStop) && pages < 400) {
                lastWindow = s, lastMessage = message, lastStop = stop;
                ++pages;
                LOG("[vr] message window %08X: page waits for the button (message %u, stop %u, next offset %d): %d letters shown%s",
                    s, message, stop, nextOffset, count, count >= kAllShown ? " (all)" : "");
            }
        }
        if (vr_text_speed() > 1 && nextOffset < 0 && count < kAllShown) {  // (only with the speed-up: the game's own by default)
            static uint32_t logged = 0;
            if (logged < 40) {
                ++logged;
                LOG("[vr] message window %08X: page at its end with %d letters shown (stop %u; counts %d %d %d %d, offsets %d %d %d %d): "
                    "all shown", s, count, ld32(s + kStopIndex), int32_t(ld32(s + kStopCounts)),
                    int32_t(ld32(s + kStopCounts + 4)), int32_t(ld32(s + kStopCounts + 8)),
                    int32_t(ld32(s + kStopCounts + 12)), int32_t(ld32(s + kStopOffsets)),
                    int32_t(ld32(s + kStopOffsets + 4)), int32_t(ld32(s + kStopOffsets + 8)),
                    int32_t(ld32(s + kStopOffsets + 12)));
            }
            st32(s + kCharacterCount, uint32_t(kAllShown));
        }
    }
#endif
    f_026FD1DC_orig(c);
}

// a page's stop waiting for A (hdMsgWindow_updateStop 026FCEE0): its first steps take no A in VR (stop_settling above)
extern "C" void hook_026FCEE0(Cpu* c) {
#ifdef WWHD_VR
    const uint32_t s = c->r[3];
    if (vr::active() && s) hold_input_while_settling(s, "stop");
#endif
    f_026FCEE0_orig(c);
}

// (diagnostics, the same) a message window's typing (hdMsgWindow_updateOpen 026FCC10): each window's letter count going
// back to the start, logged with the window, the count it had and the message: a page typed whole was seen typed again
// from the start in the two videos, and that second typing is what stopped
extern "C" void hook_026FCC10(Cpu* c) {
#ifdef WWHD_VR
    constexpr uint32_t kCharacterCount = 0x90, kMessageId = 0x5C;
    const uint32_t s = c->r[3];
    const int32_t before = s ? int32_t(ld32(s + kCharacterCount)) : 0;
    // The text typed faster in VR (WWHD_VR_TEXT_SPEED, 1..8; default 1, the game's own: vr_text_speed): the step's typing speed added
    // (speed - 1) more times to the window's accumulator (mTypingAccumulator +0x94) before the game types, so its own
    // hdMsgWindow_advanceTyping counts that many more letters and its own update (the window's 0x434, which turns each
    // letter white as it is spoken) runs over all of them, with the game's own pauses and page ends (mStopCounts).
    // Before (2026-11-05 .. 11-09) the count was raised after the game's update: the letters jumped over were never
    // turned white, and a page looked done with half of it dark (seen in the headset, 2026-11-09: "the text is
    // forwarding too fast and not all text gets highlighted white"; also the Quest reports of the white stopping)
    if (vr::active() && s) {
        const int speed = vr_text_speed();
        constexpr uint32_t kTypingAccumulator = 0x94, kTypingSpeed = 0x980;
        constexpr int32_t kAllShown = 0x14F;
        const uint32_t core = ld32(GD(0x101F4B5C));  // the message windows' core (l_windowCore), its mTypingSpeed +0x980
        if (speed > 1 && core && before < kAllShown) {
            const float step = float(ldf32(core + kTypingSpeed));
            const float acc = float(ldf32(s + kTypingAccumulator));
            if (std::isfinite(step) && step > 0.0f && step < 64.0f && std::isfinite(acc)) {
                stf32(s + kTypingAccumulator, acc + step * float(speed - 1));
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    LOG("[vr] conversation text: typed %dx as fast in VR (WWHD_VR_TEXT_SPEED), by the game's own typing", speed);
                }
            }
        }
    }
    f_026FCC10_orig(c);
    // (diagnostics, 2026-11-10: the text left part grey at 45 and 30 fps, right at 90) how often the typing runs for each
    // logic step and on passes without logic, the letters it adds and the typing speed, once every 150 calls. Measured at
    // 45 fps: once a step, never on passes without logic, 4 letters a call, as at 90
    if (vr::active() && s) {
        static uint64_t lastStep = ~0ull;
        static uint32_t callsThisStep = 0, perStep[4] = {}, holdCalls = 0, calls = 0, lettersAdded = 0, logs = 0;
        const uint64_t stepNow = interp::logic_steps();
        if (stepNow != lastStep) {
            if (lastStep != ~0ull) ++perStep[std::min<uint32_t>(callsThisStep, 3)];
            lastStep = stepNow;
            callsThisStep = 0;
        }
        ++callsThisStep;
        ++calls;
        if (interp::frame_is_hold()) ++holdCalls;
        const int32_t after = int32_t(ld32(s + 0x90));
        if (after > before) lettersAdded += uint32_t(after - before);
        if (calls >= 150 && logs < 60) {
            ++logs;
            const uint32_t core = ld32(GD(0x101F4B5C));
            LOG("[vr] message typing: %u calls, %u on passes without logic; steps with 1 call %u, 2 %u, 3+ %u; %u letters "
                "added; typing speed %.3f a call; drawn at %d fps",
                calls, holdCalls, perStep[1], perStep[2], perStep[3], lettersAdded, core ? double(ldf32(core + 0x980)) : 0.0,
                interp::output_fps());
            calls = holdCalls = lettersAdded = 0;
            perStep[0] = perStep[1] = perStep[2] = perStep[3] = 0;
        }
    }
    if (vr::active() && s) {
        struct Seen { uint32_t window; int32_t most; uint32_t message; };
        static Seen seen[4] = {};
        Seen* w = nullptr;
        for (auto& e : seen)
            if (e.window == s) w = &e;
        const uint32_t message = ld32(s + kMessageId);
        static uint32_t typedLogged = 0;
        if (!w) {
            static int next = 0;
            w = &seen[next++ & 3];
            *w = {s, 0, message};
            if (typedLogged++ < 400) LOG("[vr] message window %08X typing (message %u)", s, message);
        } else if (w->message != message) {
            // (each message, not only a window's first: the black-text report, 2026-11-09, had none for later talks)
            w->message = message;
            w->most = 0;
            if (typedLogged++ < 400) LOG("[vr] message window %08X typing (message %u)", s, message);
        }
        int32_t now = int32_t(ld32(s + kCharacterCount));
        // The same page typed again (Quest videos, 2026-11-05: a page typed whole and white, then typed again from the
        // start, faint, and that second typing stalled): the same window, message and page (stop index) going back
        // below what it had reached, within kRetypeSteps, keeps what it had reached, so the page stays white. A page
        // shown again later (talking again) types as the game types it
        {
            constexpr uint32_t kStopIndex = 0xE8;
            constexpr uint64_t kRetypeSteps = 90;  // (3 s)
            struct Page { uint32_t window = 0, message = ~0u, stop = ~0u; int32_t most = 0; uint64_t at = 0; };
            static Page pages[4];
            const uint32_t stopNow = ld32(s + kStopIndex);
            const uint64_t stepNow = interp::logic_steps();
            Page* p = nullptr;
            for (auto& e : pages)
                if (e.window == s) p = &e;
            if (!p) {
                static int nextPage = 0;
                p = &pages[nextPage++ & 3];
                *p = {};
            }
            if (p->window != s || p->message != message || p->stop != stopNow || stepNow > p->at + kRetypeSteps) {
                *p = {s, message, stopNow, now, stepNow};
            } else if (now < p->most && p->most > 4 && now < before) {
                static uint32_t logged = 0;
                if (logged++ < 40)
                    LOG("[vr] message window %08X: the page typed again from %d (it had %d; message %u, stop %u): kept",
                        s, now, p->most, message, stopNow);
                st32(s + kCharacterCount, uint32_t(p->most));
                now = p->most;
            } else {
                p->most = std::max(p->most, now);
                p->at = stepNow;
            }
        }
        if (now < before && before > 4) {
            static uint32_t logged = 0;
            if (logged++ < 40)
                LOG("[vr] message window %08X: typing back to %d from %d (message %u)", s, now, before, ld32(s + kMessageId));
        }
        w->most = std::max(w->most, now);
    }
#else
    f_026FCC10_orig(c);
#endif
}

// a parry's finishing cut (procBtJumpCut_init 0242FA80, procBtRollCut_init 02430078, procBtVerticalJumpCut_init
// 024306C8): in VR first person it waits for the player's own swing (vr::game::parry_cut_wait; docs/VR_CONTROLS.md,
// "Parry attacks"). Its callers try again each step while the move lasts: the jump until he lands, the roll held at its
// end, the leap until he lands; after 1.5 s the game's own cut. Returns TRUE as the cut's start does
#ifdef WWHD_VR
static bool parry_cut_waits(Cpu* c) {
    if (!vr::game::parry_cut_wait()) return false;
    c->r[3] = 1;
    return true;
}
#endif
extern "C" void hook_0242FA80(Cpu* c) {
#ifdef WWHD_VR
    if (parry_cut_waits(c)) return;
#endif
    f_0242FA80_orig(c);
}
extern "C" void hook_02430078(Cpu* c) {
#ifdef WWHD_VR
    if (parry_cut_waits(c)) return;
#endif
    f_02430078_orig(c);
}
extern "C" void hook_024306C8(Cpu* c) {
#ifdef WWHD_VR
    if (parry_cut_waits(c)) return;
#endif
    f_024306C8_orig(c);
}

// daBoomerang_sightPacket_c::draw (020CDBFC): the boomerang's lock-on sights, drawn flat at the targets' places on the
// screen (setSight projects each target with the game's own camera, the symmetric field). In the eye pictures each eye
// drew them at the same pixel of a different field, so they showed twice (reported in the headset, 2026-11-04). In VR
// they are drawn as the game's layouts are, once, into the UI layer's image (head-locked, the symmetric field: the
// targets' places as the game computed them; docs/VR_EYE_PROJECTION.md, "The game's UI as its own layer")
extern "C" void hook_020CDBFC(Cpu* c) {
#ifdef WWHD_VR
    if (stereo::requestedXR.load(std::memory_order_relaxed)) {
        gx2::emit(gx2::OP_LAYOUT_CONTENT, {0u, 1u, 0u});  // (begin: no content clip, a layout, not a photograph)
        f_020CDBFC_orig(c);
        gx2::emit(gx2::OP_LAYOUT_CONTENT, {0u, 0u});  // (end)
        return;
    }
#endif
    f_020CDBFC_orig(c);
}

// daArrow_c::setKeepMatrix (0205244C): the arrow on Link's hand, each step until shot; its procWait shoots it right
// after, along the angles this sets. In VR the arrow lies along the bow (docs/VR_AIMING.md, "The arrow along the bow"):
// its angles and place are set to that line afterwards, so it flies where its red laser points (reported in the
// headset: with the bow lowered the shots went elsewhere)
extern "C" void hook_0205244C(Cpu* c) {
#ifdef WWHD_VR
    const uint32_t arrow = c->r[3];
    f_0205244C_orig(c);
    vr::game::arrow_kept(arrow);
#else
    f_0205244C_orig(c);
#endif
}

extern "C" void hook_02506964(Cpu* c) {
#ifdef WWHD_VR
    const uint32_t cam = c->r[3], pitch = c->r[4], yaw = c->r[5];
    f_02506964_orig(c);
    const uint32_t result = c->r[3];
    vr::game::subject_angle(cam, pitch, yaw);
    c->r[3] = result;
#else
    f_02506964_orig(c);
#endif
}

// dCam_getControledAngleY (024F8018): the angle Link's stick is relative to; in VR the head's heading
extern "C" void hook_024F8018(Cpu* c) {
    f_024F8018_orig(c);
#ifdef WWHD_VR
    c->r[3] = uint32_t(int32_t(vr::game::controlled_angle_hook(int16_t(c->r[3] & 0xFFFF))));
#endif
}

// daPy_lk_c::jointCB1 (02403150), right after Link's model calc: in VR his arms posed towards the controllers
extern "C" void hook_02403150(Cpu* c) {
#ifdef WWHD_VR
    const uint32_t link = c->r[3];
    f_02403150_orig(c);
    const uint32_t result = c->r[3];
    vr::game::link_arms(link);
    c->r[3] = result;
#else
    f_02403150_orig(c);
#endif
}

// daPy_Draw (023DBBE4): Link's draw; VR's first-person presentation around it (head hidden, tracked sword)
extern "C" void hook_023DBBE4(Cpu* c) {
#ifdef WWHD_VR
    const uint32_t link = c->r[3];
    // the Free Roam Camera: his whole body not drawn (the draw skipped, its result TRUE as the game's own draw returns)
    if (vr::game::link_hidden()) {
        c->r[3] = 1;
        return;
    }
    vr::game::link_draw_begin(link);
    f_023DBBE4_orig(c);
    vr::game::link_draw_end(link);
#else
    f_023DBBE4_orig(c);
#endif
}

// daBoko_Draw (020C0E04): a weapon picked up from an enemy; in VR the one Link holds is placed at the tracked hand
// before its own draw (it may be drawn before Link, whose draw places it too)
extern "C" void hook_020C0E04(Cpu* c) {
#ifdef WWHD_VR
    vr::game::weapon_drawing(c->r[3]);
#endif
    f_020C0E04_orig(c);
}

// dPa_control_c::draw(this, info, group) (025A83AC): in VR the 2D particle groups are not drawn (vr_ui.h
// draws_particle_group: the HUD's sparkles floated in the view as stars), nor the heat haze while dialogue or a
// cutscene is shown (it bent the text)
extern "C" void hook_025A83AC(Cpu* c) {
#ifdef WWHD_VR
    if (!vr::ui::draws_particle_group(c->r[5], vr::ui::state())) return;
#endif
    f_025A83AC_orig(c);
}

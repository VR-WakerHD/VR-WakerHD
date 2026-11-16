// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// The Vulkan renderer's side of VR mode: see vr_vulkan.h.
#include "vr_vulkan.h"
#include "platform/core_groups.h"

#include "deferred_dispatch.h"
#include "pod_commands.h"
#include "texture_pack_vk.h"
#include "platform/host.h"
#include "stereo.h"
#include "runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#ifdef WWHD_VR
#include <cmath>
#include "../display_modes.h"
#include "../../aspect.h"
#include "../../interp.h"
#include "../../interp_pacing.h"
#include "../../dynres.h"
#include "vr/vr_dynres.h"
#include "vr/vr.h"
#include "overlay/hostui.h"
#include "settings.h"
#include "vr/vr_camera.h"
#include "vr/vr_item_wheel_draw.h"
#include "vr/vr_wrist_hud.h"
#include "vr/vr_laser.h"
#include "present.h"
#include "vr_upscale.h"
#include "vr_cas.h"
#include "vr_sgsr2.h"
#include "vr_fsr1.h"
#include "motion_pass.h"
#include "vr_framesynth.h"
#include "vr_genframe.h"
#include "fov_diag.h"
#include "vr/vr_framesynth.h"
#include "ngx_vk.h"
#include "vr/vr_upscale.h"
#include <unordered_map>
namespace gx2 { void lock_vsync_to_display(int64_t period); void delay_vsync_grid(int64_t ns); uint64_t swaps_executed(); }  // (gx2_core.cpp)
namespace vr::game { bool menu_open(); bool name_entry_open(); bool hands_layer_view(bool force); }  // (vr_game.cpp: the VR menu, the new save's name, first-person play)
#endif

namespace gfxvk::vrvk {
#ifndef WWHD_VR
VkResult create_instance(const VkInstanceCreateInfo& ci, VkInstance* out) { return vkCreateInstance(&ci, nullptr, out); }
int preferred_device(const std::vector<VkPhysicalDevice>&) { return -1; }
VkResult create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo& ci, VkDevice* out) {
    return vkCreateDevice(physical, &ci, nullptr, out);
}
void device_ready() {}
void frame_begin() {}
bool skip_desktop_gamepad() { return false; }
bool skip_window_tv() { return false; }
void record_eyes(VkCommandBuffer) {}
void frame_end() {}
bool eyes_pending() { return false; }
int64_t headset_frame_due() { return 0; }
void headset_frame() {}
void planned_repeats() {}
bool hands_layer_wanted() { return false; }
void shutdown() {}
int smoke(int) { return 0; }
#else
namespace {
bool g_tried = false;       // OpenXR init was attempted (once per start)
bool g_xrInstance = false;  // the Vulkan instance came from the runtime
VkInstance g_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_physical = VK_NULL_HANDLE;
bool g_frame = false, g_shouldRender = false, g_recorded = false;
uint32_t g_frameViews = 0;
bool g_handsRecorded = false;  // (docs/VR_HANDS_LAYER_PLAN.md step 3) this frame's hands-layer images written
// (2026-11-14, the Tower of the Gods run: blinking, rainbow hands) both eyes' halves written: the layer may be shown. An
// eye's pass that failed left its half of the shared image as it was (an older frame's, or never written: noise of
// every colour), and the layer was sent with it. Now the images are still released but the layer is not shown
bool g_handsShow = false;
// (diagnostics, every 10 s while the layer is on) frames whose hands went into the layer's image but were not shown, and why
struct HandsMiss { uint32_t frames = 0, oneEye = 0, passFailed = 0, noImage = 0, noDepth = 0; } g_handsMiss;
// (the eyepiece's black, eye_part) the field the game's camera and the UI layer's quad cover, and each eye's own field
// its picture covers (each eye drawn with its own projection): the eyepiece is a layout, in the UI layer
vr::Fov g_drawnFov, g_eyeFov[2];
bool g_eyeFovs = false;
// the eyepiece's area (shares of the UI layer's width and height, centred: vr::eyepiece) in eye `eye`'s picture: its
// field is off-centre across (each eye sees more to its own side), so the area sits off its centre there. Linear in
// the tangents, as both the quad and the eye's picture are; the eyes' small parallax at the quad's 3.5 m left out.
// False: the picture covers the drawn field (no own eye projection), the area is centred
bool eye_part(int eye, float ew, float eh, float& x0, float& x1, float& y0, float& y1) {
    if (!g_eyeFovs) return false;
    const vr::Fov& f = g_eyeFov[eye];
    const float l = std::tan(f.left), r = std::tan(f.right), u = std::tan(f.up), d = std::tan(f.down);
    const float tx = ew * std::tan(g_drawnFov.right), ty = eh * std::tan(g_drawnFov.up);
    if (!(r > l) || !(u > d) || !std::isfinite(tx) || !std::isfinite(ty)) return false;
    x0 = std::clamp((-tx - l) / (r - l), 0.0f, 1.0f), x1 = std::clamp((tx - l) / (r - l), 0.0f, 1.0f);
    y0 = std::clamp((u - ty) / (u - d), 0.0f, 1.0f), y1 = std::clamp((u + ty) / (u - d), 0.0f, 1.0f);
    return x1 > x0 && y1 > y0;
}
// the session's first running frame did its start-up (refresh rate, frame interpolation)
bool g_started = false;
int g_savedInterpMode = -1, g_savedInterpFps = 0;  // frame interpolation as the player had it (restored at the end)
// the GamePad screen as a panel: shown this frame, copied this frame, where (world-locked while it is up)
bool g_panelShown = false, g_panelRecorded = false;
vr::xr::Quad g_panel;
// the VR menu on the panel (docs/VR_MENU.md): its picture's size, and a view of each of the panel's images it is
// drawn into (made on first use, until the session ends)
bool g_panelMenu = false;
constexpr uint32_t kMenuPanelWidth = 1600, kMenuPanelHeight = 900;
constexpr float kMenuPanelScale = 2.0f;  // the menu's panel: twice the GamePad screen's (asked for in the headset)
std::unordered_map<VkImage, VkImageView> g_panelViews;
void destroy_panel_views() {
    if (!g_panelViews.empty()) note_descriptor_targets_destroyed();
    for (auto& [image, view] : g_panelViews)
        if (view) vkDestroyImageView(R.device, view, nullptr);
    g_panelViews.clear();
}
// the item wheel (docs/VR_ITEM_WHEEL.md): its picture copied into its quad's image this frame, and where
bool g_wheelRecorded = false;
// (docs/VR_EYE_PROJECTION.md) the game's UI layer's picture copied this frame (each eye drawn with its own projection)
bool g_uiRecorded = false;
constexpr float kUiDistance = 3.5f;  // metres ahead of the eyes: the UI layer's quad, covering the symmetric field (headset: 1.5 and 2.5 too close)
vr::xr::Quad g_wheel;
// the wrist HUD (docs/VR_WRIST_HUD.md): its picture copied this frame, and its panels
bool g_wristRecorded = false;
vr::xr::SubQuad g_laser[2];  // the aiming laser's beam and dot (docs/VR_AIMING.md)
bool g_laserRecorded = false;
int g_eyeRect[2][2] = {};  // (record_eyes) the part of each eye's image its picture was copied into at its own size; 0: all
vr::xr::SubQuad g_wrist[vr::kWristPanels];
// (Session::cpu_boost) when the CPU's Boost, asked for while the game was stalled (headset_frame), ends
std::chrono::steady_clock::time_point g_boostUntil{};
// the last frame the headset was given (frame_end): headset_frame shows it again while the game has no new one
struct Shown {
    bool valid = false, drawn = false, wrist = false, laser = false, ui = false;
    bool hands = false;  // (docs/VR_HANDS_LAYER_PLAN.md) the hands layer's images released with it: shown again unturned
    vr::xr::Views views;
    vr::xr::Quad panel, wheel, uiQuad;  // (uiQuad: the game's UI layer, docs/VR_EYE_PROJECTION.md; shown again with the eyes)
    vr::xr::SubQuad wristQuads[vr::kWristPanels], laserQuads[2];
    float turnRate = 0;  // (turn smoothing) the view's turn rate when it went out (vr::view_turn_rate), radians a second
    vr::Vec3 velocity;   // (movement smoothing) the view's velocity when it went out (vr::view_velocity), tracking metres a second
    vr::Vec3 pivot;          // (generated frames) the stick turn's pivot when it went out (vr::turn_pivot), tracking metres
    float pivotLimit = 0;    // (and the head's sideways offset from it held within this)
    bool havePivot = false;
    // the refresh it was actually given for (its xrWaitFrame's predicted display time). Its views were located for a
    // later moment (views.time: the game predicts its camera about a refresh past the frame's own refresh): the frames
    // made after it are moved on from this one, so each frame, real or made, is the same time ahead of its own refresh
    XrTime shownAt = 0;
} g_shown;
// the time a frame made after the last real one is moved on by: from the real frame's own refresh to this frame's
// (2026-11-12, a headset run with generated frames: measured from the real frame's views.time they came out 0.8 ms after
// it, turned 0.02 degrees while the stick turned 1.5 a frame: the first made frame repeated the real one and the next
// jumped, "x3 worse" than without them)
// (callers clamp it to -3 .. 6 refreshes: 0 .. 3 before. A real frame drawn for a moment 22 ms or more before the
// refresh it landed on (a headset sequence capture, after a hitch) put both its generated frames past 3 refreshes: both
// clamped to the same moment, the second stood still and the next real frame jumped two refreshes' turn at once)
double since_shown(XrTime displayTime) {
    // (2026-11-12) from the moment the real frame was drawn for (its views' time), not the refresh it landed on: a
    // headset run had a third of the real frames land a refresh before or after the one they were drawn for (their
    // lead's spread 6.5 ms, up to 22 ms either way), and the frames made from them carried the error on
    const XrTime base = g_shown.views.valid && g_shown.views.time ? g_shown.views.time : g_shown.shownAt;
    return double(displayTime - base) * 1e-9;
}
XrTime g_lastSubmittedDisplay = 0;  // (the display time of the last frame given to the headset, real or generated)
// (diagnostics, 2026-11-12, "[vr] repeated frames' timing (10 s)") Turn smoothing's frames, plain (re-sent, show_again)
// and with the near field held (drawn, show_generated): how long each took from its frame begun to its frame ended, and
// how many landed more than one refresh after the frame before them (the refresh between shown twice). A headset run
// had the held ones look worse than the plain ones with the far view's pixels identical
struct RepeatTiming {
    uint64_t n = 0, late = 0;
    double sumMs = 0, maxMs = 0;
    void add(double ms, bool wasLate) { ++n, late += wasLate, sumMs += ms, maxMs = std::max(maxMs, ms); }
};
RepeatTiming g_rtPlain, g_rtHeld;
std::chrono::steady_clock::time_point g_rtSince = std::chrono::steady_clock::now();
bool repeat_late(XrTime shown) {
    const XrDuration period = vr::session().refresh_period();
    const bool late = g_lastSubmittedDisplay && period > 0 && double(shown - g_lastSubmittedDisplay) > 1.5 * double(period);
    return late;
}
void repeat_timing_log() {
    const auto now = std::chrono::steady_clock::now();
    if (now - g_rtSince < std::chrono::seconds(10)) return;
    if (g_rtPlain.n || g_rtHeld.n) {
        auto line = [](const RepeatTiming& t) {
            char b[160];
            std::snprintf(b, sizeof b, "%llu, %.2f ms each on average (%.2f at most), %llu a refresh late",
                          (unsigned long long)t.n, t.n ? t.sumMs / double(t.n) : 0.0, t.maxMs, (unsigned long long)t.late);
            return std::string(b);
        };
        LOG("[vr] repeated frames' timing (10 s): plain %s; near field held %s", line(g_rtPlain).c_str(), line(g_rtHeld).c_str());
    }
    g_rtPlain = g_rtHeld = RepeatTiming{};
    g_rtSince = now;
}
// (diagnostics, every 10 s: "[vr] frame evenness (10 s)") check 1: the view's turn from one refresh to the next over every
// frame shown, real or generated (the heading each was drawn at, a generated one's plus the turn drawn into it), while
// it turns: smooth is the same step every refresh; check 2: how far ahead of its own refresh each real frame was drawn
// (its views' time less its refresh's)
struct Evenness {
    bool have = false;
    float lastYaw = 0;
    XrTime lastAt = 0;
    uint64_t steps = 0, realSteps = 0, genSteps = 0;
    double sum = 0, sumSq = 0, lo = 1e9, hi = -1e9, realSum = 0, genSum = 0;
    uint64_t leads = 0;
    double leadSum = 0, leadSq = 0, leadLo = 1e9, leadHi = -1e9;
    // (2026-11-12) the steady stick: each refresh's turn as shown against the stick's rate times a refresh, while the rate
    // holds (within 2 %, over 10 degrees a second): smooth is 1.00 every refresh. Off is a refresh's step 25 % or more
    // from it (a frame shown at another moment than it was drawn for)
    float lastRate = 0;
    uint64_t steady = 0, steadyOff = 0, steadyBack = 0;
    double steadySum = 0, steadySq = 0;
    std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
} g_even;
void note_shown(float yaw, XrTime at, bool generated, float rate) {
    const XrDuration period = vr::session().refresh_period();
    if (g_even.have && period > 0 && at > g_even.lastAt) {
        const double refreshes = std::max(1.0, std::round(double(at - g_even.lastAt) / double(period)));
        const double step = std::remainder(double(yaw - g_even.lastYaw), 2.0 * 3.14159265358979) * 57.29578 / refreshes;
        const double expect = double(rate) * 57.29578 * double(period) * 1e-9;
        if (std::fabs(rate) > 10.0f / 57.29578f && std::fabs(rate - g_even.lastRate) <= 0.02f * std::fabs(rate)) {
            const double ratio = step / expect;
            if (std::fabs(ratio) < 5.0) {
                ++g_even.steady;
                g_even.steadySum += ratio, g_even.steadySq += ratio * ratio;
                if (std::fabs(ratio - 1.0) >= 0.25) ++g_even.steadyOff;
                if (ratio < 0) ++g_even.steadyBack;
            }
        }
        if (std::fabs(step) > 0.05 && std::fabs(step) < 20.0) {  // (turning; a snap or a cut left out)
            const double a = std::fabs(step);
            ++g_even.steps;
            g_even.sum += a, g_even.sumSq += a * a, g_even.lo = std::min(g_even.lo, a), g_even.hi = std::max(g_even.hi, a);
            if (generated) ++g_even.genSteps, g_even.genSum += a;
            else ++g_even.realSteps, g_even.realSum += a;
        }
    }
    g_even.have = true;
    g_even.lastYaw = yaw;
    g_even.lastAt = at;
    g_even.lastRate = rate;
    const auto now = std::chrono::steady_clock::now();
    if (now - g_even.since >= std::chrono::seconds(10)) {
        if (g_even.steps || g_even.leads) {
            const double n = double(std::max<uint64_t>(g_even.steps, 1)), mean = g_even.sum / n;
            const double sd = std::sqrt(std::max(0.0, g_even.sumSq / n - mean * mean));
            const double ln = double(std::max<uint64_t>(g_even.leads, 1)), lm = g_even.leadSum / ln;
            const double lsd = std::sqrt(std::max(0.0, g_even.leadSq / ln - lm * lm));
            LOG("[vr] frame evenness (10 s): turn per refresh %.2f degrees on average, spread %.2f (%.0f%%), %.2f to %.2f, over "
                "%llu refreshes; real frames' steps %.2f, generated frames' %.2f; real frames drawn %.1f ms ahead of their "
                "refresh on average (spread %.1f, %.1f to %.1f, %llu frames)",
                mean, sd, mean > 0 ? 100.0 * sd / mean : 0.0, g_even.steps ? g_even.lo : 0.0, g_even.steps ? g_even.hi : 0.0,
                (unsigned long long)g_even.steps, g_even.realSteps ? g_even.realSum / double(g_even.realSteps) : 0.0,
                g_even.genSteps ? g_even.genSum / double(g_even.genSteps) : 0.0, lm, lsd, g_even.leads ? g_even.leadLo : 0.0,
                g_even.leads ? g_even.leadHi : 0.0, (unsigned long long)g_even.leads);
            if (g_even.steady) {
                const double sn = double(g_even.steady), sm = g_even.steadySum / sn;
                const double ssd = std::sqrt(std::max(0.0, g_even.steadySq / sn - sm * sm));
                LOG("[vr] steady stick turn (10 s): each refresh's turn %.2f of the stick's on average, spread %.0f%%, over %llu "
                    "refreshes; %llu (%.0f%%) off by a quarter or more, %llu stepping back (smooth: 1.00, 0%%, none off)", sm,
                    100.0 * ssd, (unsigned long long)g_even.steady, (unsigned long long)g_even.steadyOff,
                    100.0 * double(g_even.steadyOff) / sn, (unsigned long long)g_even.steadyBack);
            }
        }
        const bool have = g_even.have;
        const float lastYaw = g_even.lastYaw, lastRate = g_even.lastRate;
        const XrTime lastAt = g_even.lastAt;
        g_even = Evenness{};
        g_even.have = have, g_even.lastYaw = lastYaw, g_even.lastAt = lastAt, g_even.lastRate = lastRate;
    }
}
// the game's own frames (frame_end, not headset_frame): when the last one went out, and the recent interval between
// them (ns, a running average), so that headset_frame waits for a real stall rather than the next frame
std::chrono::steady_clock::time_point g_lastGameFrame{};
double g_gameFrameInterval = 0;
// (Turn smoothing) the refreshes after each game frame it fills: Capped 45 and Steady 30 by the setting (the headset's
// rate over the game's: 1 at 45 fps on 90 Hz, 2 at 30); Auto Mode by the game's measured pace (asked for, 2026-11-12:
// "in auto mode ... only have it work when fps dips low"): 0 while the game keeps up with the headset (a real 90: nothing
// runs, nothing is added), 1 at 45, 2 at 30. Capped 90 and Unlocked: 0
// (Auto Mode's flicker hold, asked for 2026-11-14: steady hands went on and off 10 times in seconds while the game
// dipped just under 90 and recovered) Auto Mode's "below the headset's rate" held: on at once, off only after the game
// has kept up for kAutoLowHold. Decided once a game frame (frame_end, render thread); steady hands follows it
// (hands_layer_wanted). Smooth Camera while Turning's own frames follow the game's real pace (low_rate_repeats), so they
// never take a refresh a real frame needs
constexpr auto kAutoLowHold = std::chrono::seconds(1);
std::atomic<bool> g_autoLowHeld{false};
std::chrono::steady_clock::time_point g_autoLowOkSince{};
int low_rate_repeats(double hz, int fps);
std::chrono::steady_clock::time_point g_autoLowSettleUntil{};
void update_auto_low_hold(double hz, std::chrono::steady_clock::time_point now) {
    // (2026-11-14: the VR menu pauses the game, its slow frames read as a low rate, and steady hands went on while Auto
    // was being picked there, a headset run's log) the menu's frames and the first second after it closes are not judged:
    // the decision stays as it was (the frames' measured pace takes that long to settle again)
    if (vr::game::menu_open()) {
        g_autoLowSettleUntil = now + std::chrono::seconds(1);
        return;
    }
    if (now < g_autoLowSettleUntil) return;
    const bool low = vr::config().frameRate == vr::FrameRate::Auto && low_rate_repeats(hz, 0) > 0;
    const bool was = g_autoLowHeld.load(std::memory_order_relaxed);
    if (low) {
        g_autoLowOkSince = {};
        if (!was) g_autoLowHeld.store(true, std::memory_order_relaxed);
        return;
    }
    if (!was) return;
    if (g_autoLowOkSince == std::chrono::steady_clock::time_point{}) g_autoLowOkSince = now;
    else if (now - g_autoLowOkSince >= kAutoLowHold) {
        g_autoLowHeld.store(false, std::memory_order_relaxed);
        g_autoLowOkSince = {};
    }
}
// (2026-11-14, asked for: "for auto mode we can use refresh filling as the solution") Auto Mode with the runtime's frame
// synthesis: while frames carry motion vectors the game is paced at half the refresh rate (45 at 90 Hz) and the runtime
// makes one frame between two. When the game cannot hold that (more than 2.5 refreshes a frame: under 36 fps at 90 Hz),
// one synthesized frame cannot fill the gap, so frame synthesis is held (vr_framesynth::set_held, frame_begin) and refresh
// filling (planned_repeats) gives those refreshes the last frame again, as at Steady 30. Held after 1 s slow; used again
// after 5 s at 2.2 refreshes a frame or less (41 fps at 90 Hz). Held again within 10 s of being used again,
// the next wait doubles (up to 60 s): the motion pass, paused while held, costs time again when used, and must not make
// it flip back and forth. The VR menu (the game paused) and the second after it are not judged. Decided after each game
// frame (frame_end, render thread). Only where the session has frame synthesis swapchains (a Quest, the Steam Frame):
// PC VR never does, so nothing changes there
std::atomic<bool> g_autoSynthHeld{false};
void update_auto_synth_hold(double hz, bool synthesisSwapchains, std::chrono::steady_clock::time_point now) {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point slowSince{}, okSince{}, settleUntil{}, usedAgainAt{};
    static double releaseSecs = 5.0;
    static uint64_t holds = 0;
    const bool applies = vr::config().frameRate == vr::FrameRate::Auto && synthesisSwapchains && hz > 0;
    const bool held = g_autoSynthHeld.load(std::memory_order_relaxed);
    if (!applies) {
        if (held) {
            g_autoSynthHeld.store(false, std::memory_order_relaxed);
            LOG("[vr] Auto Mode frame synthesis hold: off (the frame rate is no longer Auto Mode)");
        }
        slowSince = okSince = {};
        return;
    }
    if (vr::game::menu_open()) {
        settleUntil = now + std::chrono::seconds(1);
        slowSince = okSince = {};
        return;
    }
    if (now < settleUntil || !(g_gameFrameInterval > 0)) return;
    const double perFrame = g_gameFrameInterval * hz * 1e-9;  // refreshes a game frame
    const double fps = 1e9 / g_gameFrameInterval;
    if (!held) {
        if (perFrame <= 2.5) {
            slowSince = {};
            return;
        }
        if (slowSince == Clock::time_point{}) slowSince = now;
        if (now - slowSince < std::chrono::seconds(1)) return;
        releaseSecs = usedAgainAt != Clock::time_point{} && now - usedAgainAt < std::chrono::seconds(10)
                          ? std::min(releaseSecs * 2.0, 60.0)
                          : 5.0;
        g_autoSynthHeld.store(true, std::memory_order_relaxed);
        slowSince = okSince = {};
        ++holds;
        LOG("[vr] Auto Mode frame synthesis hold: on (the game at %.1f fps, under half the %.0f Hz refresh rate, for 1 s; "
            "the refreshes between its frames filled instead; used again after %.0f s at %.0f fps or more; %llu holds since "
            "the start)",
            fps, hz, releaseSecs, hz / 2.2, (unsigned long long)holds);
        return;
    }
    if (perFrame > 2.2) {
        okSince = {};
        return;
    }
    if (okSince == Clock::time_point{}) okSince = now;
    if (now - okSince < std::chrono::duration<double>(releaseSecs)) return;
    g_autoSynthHeld.store(false, std::memory_order_relaxed);
    okSince = {};
    usedAgainAt = now;
    LOG("[vr] Auto Mode frame synthesis hold: off (the game at %.1f fps for %.0f s: frame synthesis used again, the game "
        "paced at half the refresh rate)",
        fps, releaseSecs);
}
// (diagnostics, asked for 2026-11-14: after switching to Capped 90 the game stayed near 47 fps for 10-20 s once) a
// frame rate change: how long from the VR menu closing (or the change, if it was not open) until the game draws at its
// new rate (95 % of it, held 0.5 s), with the game's steps a second and the hands layer meanwhile; or not reached in 30 s
struct RateSwitch {
    int mode = -1;
    bool pending = false;
    double target = 0;
    std::chrono::steady_clock::time_point from{}, okSince{};
    uint64_t stepsFrom = 0;
    int handsFrames = 0, frames = 0;
} g_rateSwitch;
void note_rate_switch(double hz, std::chrono::steady_clock::time_point now) {
    auto& r = g_rateSwitch;
    const vr::FrameRate mode = vr::config().frameRate;
    if (r.mode != int(mode)) {
        const bool first = r.mode < 0;
        r = {};
        r.mode = int(mode);
        if (first || hz <= 0) return;
        r.target = mode == vr::FrameRate::Full || mode == vr::FrameRate::Auto ? hz
                 : mode == vr::FrameRate::Half ? hz / 2
                 : mode == vr::FrameRate::Original ? 30.0 : 0.0;
        r.pending = r.target > 0;
        r.from = now;
        r.stepsFrom = interp::logic_steps();
        return;
    }
    if (!r.pending) return;
    if (vr::game::menu_open()) {  // (measured from the menu's closing: the game is paused while it is open)
        r.from = now;
        r.stepsFrom = interp::logic_steps();
        r.okSince = {};
        r.handsFrames = r.frames = 0;
        return;
    }
    ++r.frames;
    if (g_autoLowHeld.load(std::memory_order_relaxed) || hands_layer_drawn(0)) ++r.handsFrames;
    const double fps = g_gameFrameInterval > 0 ? 1e9 / g_gameFrameInterval : 0;
    const double secs = std::chrono::duration<double>(now - r.from).count();
    const auto steps = [&] { return secs > 0 ? double(interp::logic_steps() - r.stepsFrom) / secs : 0.0; };
    if (fps >= 0.95 * r.target) {
        if (r.okSince == std::chrono::steady_clock::time_point{}) r.okSince = now;
        else if (now - r.okSince >= std::chrono::milliseconds(500)) {
            const double took = std::chrono::duration<double>(r.okSince - r.from).count();
            LOG("[vr] frame rate switch: %.0f fps reached %.2f s after the menu closed (game %.1f steps/s meanwhile, the "
                "hands layer on in %d of %d frames)", r.target, took, steps(), r.handsFrames, r.frames);
            r.pending = false;
        }
    } else {
        r.okSince = {};
        if (secs >= 30) {
            LOG("[vr] frame rate switch: %.0f fps not reached in 30 s (at %.1f fps; game %.1f steps/s, the hands layer on in "
                "%d of %d frames)", r.target, fps, steps(), r.handsFrames, r.frames);
            r.pending = false;
        }
    }
}
int low_rate_repeats(double hz, int fps) {
    const vr::FrameRate mode = vr::config().frameRate;
    if (mode == vr::FrameRate::Half || mode == vr::FrameRate::Original)
        return hz > 0 && fps > 0 ? std::clamp(int(std::lround(hz / double(fps))) - 1, 0, 3) : 0;
    if (mode == vr::FrameRate::Auto && hz > 0 && g_gameFrameInterval > 0)
        return std::clamp(int(std::lround(g_gameFrameInterval * hz * 1e-9)) - 1, 0, 3);
    return 0;
}
// diagnostics (every 10 s): frames, how long xrWaitFrame waited, frames with the game camera's views
struct Stats {
    std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
    uint64_t begun = 0, submitted = 0, withViews = 0, renderFrame = 0, logicSteps = 0, keptAlive = 0, viewsReused = 0;
    double waitMs = 0;
} g_stats;
// frame timing (every 10 s, docs/VR_PERFORMANCE.md): per frame shown, without allocating per frame
struct FrameTimes {
    std::vector<float> interval, wait, cpu, display, game;  // ms, ms, ms, display periods, logic steps
    // the VR submission's parts, ms: the eye images acquired (xrWaitSwapchainImage), the graphics queue taken in
    // frame_end, the images released and xrEndFrame (docs/VR_PERFORMANCE.md, "Where a VR frame's time goes")
    std::vector<float> acquire, queue, submit;
    // (the end-frame thread) the images released alone, and the render thread's wait for the thread at frame_begin
    std::vector<float> release, join;
    std::chrono::steady_clock::time_point lastEnd{}, begun{};
    XrTime lastDisplay = 0;
    double lastGame = -1;
    // frames that reached the headset on another refresh than their interval says (near its deadline: lock_to_headset)
    std::chrono::steady_clock::time_point slipSince{}, lastDelay{};
    int slips = 0;
    FrameTimes() {
        for (auto* v : {&interval, &wait, &cpu, &display, &game, &acquire, &queue, &submit, &release, &join}) v->reserve(4096);
    }
    void clear() {
        for (auto* v : {&interval, &wait, &cpu, &display, &game, &acquire, &queue, &submit, &release, &join}) v->clear();
    }
} g_times;
float percentile(std::vector<float>& v, float p) {
    if (v.empty()) return 0;
    const size_t k = std::min(v.size() - 1, size_t(p * float(v.size() - 1) + 0.5f));
    std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(k), v.end());
    return v[k];
}

vr::xr::Session& xs() { return vr::session(); }

// The frame's end on its own thread (docs/VR_PERFORMANCE.md, "xrEndFrame on its own thread"): the swapchain images
// released and xrEndFrame, under the graphics queue's lock as before, while the render thread goes on to the next
// frame's draws. A Quest 3 (standalone) spent 13 to 17 ms a frame there, the runtime waiting for the GPU, and the
// render thread waited with it: the CPU's and the GPU's work for a frame came one after the other (about 30 fps).
// OpenXR's order is kept: frame_begin waits for the thread before xrWaitFrame, so each xrEndFrame comes before the
// next frame's xrWaitFrame and xrBeginFrame, and its images are released before they are acquired again. Everything
// it reads is copied into its job or written only after that wait (the views, the quads, the synthesis images).
// On by default (PS VR2, 2026-11-03: the render thread never waited for it, 87 to 89 fps steady); WWHD_VR_END_FRAME_THREAD=0
// keeps the frame's end on the render thread
namespace end_thread {
bool on() {
    static const bool v = [] {
        const char* e = std::getenv("WWHD_VR_END_FRAME_THREAD");
        const bool yes = !(e && e[0] == '0');
        LOG("[vr] the frame's end (images released, xrEndFrame): %s",
            yes ? "on its own thread (WWHD_VR_END_FRAME_THREAD=0 keeps it on the render thread)"
                : "on the render thread (WWHD_VR_END_FRAME_THREAD=0)");
        return yes;
    }();
    return v;
}
struct Job {
    vr::xr::Views views;
    bool drawn = false, eyes = false, panel = false, wheel = false, wrist = false, laser = false, ui = false;
    bool hands = false;  // (docs/VR_HANDS_LAYER_PLAN.md step 3) the hands layer's images written this frame
    bool handsShow = false;  // (g_handsShow) and both halves written: the layer shown
    vr::xr::Quad panelQuad, wheelQuad, uiQuad;
    vr::xr::SubQuad wristQuads[vr::kWristPanels], laserQuads[2];
};
std::mutex g_mutex;
std::condition_variable g_cv;
Job g_job;
bool g_queued = false, g_busy = false;
// the last job's timing (ms), taken by the render thread at the next wait
float g_queueMs = 0, g_releaseMs = 0, g_submitMs = 0;
bool g_timed = false;

// the frame's end itself: on this thread, or on the render thread with the switch off
void end(const Job& j, float& queueMs, float& releaseMs, float& submitMs) {
    const auto ms = [](auto a, auto b) { return float(std::chrono::duration<double, std::milli>(b - a).count()); };
    // the runtime uses the graphics queue in release and end frame: keep the renderer's submissions out
    const auto queueStart = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> queueLock(graphics_queue_mutex());
    const auto submitStart = std::chrono::steady_clock::now();
    if (j.eyes) {
        xs().release(0);
        xs().release(1);
    }
    vr_framesynth::release_images();  // (the motion vector and depth images written this frame)
    if (j.panel) xs().release(vr::xr::Session::kPanel);
    if (j.wheel) xs().release(vr::xr::Session::kWheel);
    if (j.wrist) xs().release(vr::xr::Session::kWrist);
    if (j.laser) xs().release(vr::xr::Session::kLaser);
    if (j.ui) xs().release(vr::xr::Session::kUi);
    if (j.hands) {
        xs().release(vr::xr::Session::kHands);
        if (!vr::xr::hands_shared()) xs().release(vr::xr::Session::kHands + 1);
    }
    const auto released = std::chrono::steady_clock::now();
    vr::xr::Quad panel = j.panelQuad, wheel = j.wheelQuad, ui = j.uiQuad;
    xs().end_frame(j.views, j.drawn, &panel, &wheel, j.wrist ? j.wristQuads : nullptr, j.wrist ? vr::kWristPanels : 0,
                   j.laser ? j.laserQuads : nullptr, j.laser ? 2 : 0, j.ui ? &ui : nullptr,
                   j.hands && j.handsShow ? &j.views : nullptr);
    const auto done = std::chrono::steady_clock::now();
    queueMs = ms(queueStart, submitStart);
    releaseMs = ms(submitStart, released);
    submitMs = ms(submitStart, done);
}

// (docs/VR_STREAMING_AUDIT.md, finding 12; log only) the runtime's frame smoothing: VDXR reports its frame period as two
// refreshes while Virtual Desktop's Synchronous Spacewarp runs (any runtime that asks for frames more than a refresh
// apart reads the same). Logged when it starts and stops (the first 30 changes of a session) and every 10 s with any,
// so a log shows whether head jitter or uneven turns line up with it. On this thread, after the frame's end: nothing on
// the render thread, no lock (two atomics the frame's xrWaitFrame set)
void note_runtime_smoothing() {
    const XrDuration period = xs().predicted_period(), refresh = xs().refresh_period();
    if (period <= 0 || refresh <= 0) return;
    const double ratio = double(period) / double(refresh);
    const bool on = ratio > 1.5;
    static bool was = false;
    static int changes = 0;
    static uint64_t frames = 0, onFrames = 0, switches = 0;
    static auto since = std::chrono::steady_clock::now();
    ++frames;
    onFrames += on;
    if (on != was) {
        ++switches;
        if (++changes <= 30)
            LOG("[vr] the runtime's frame smoothing: %s", on
                ? "on (it asks for frames %.0f refreshes apart: Virtual Desktop's Synchronous Spacewarp, or the runtime's own)"
                : "off (frames a refresh apart again)", ratio);
        was = on;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - since >= std::chrono::seconds(10)) {
        if (onFrames || switches)
            LOG("[vr] the runtime's frame smoothing (10 s): on in %llu of %llu frames (%.0f%%), switched %llu times",
                (unsigned long long)onFrames, (unsigned long long)frames, 100.0 * double(onFrames) / double(frames),
                (unsigned long long)switches);
        frames = onFrames = switches = 0;
        since = now;
    }
}

void loop() {
    // as the render and record threads (above normal, no power throttling): a normal-priority thread was put off
    // or moved to a slow core while they ran, and its late xrEndFrame missed the headset's deadline (PS VR2,
    // 2026-11-02: lower frame rate, jitter and stutter)
    host::boost_thread_priority();
    core_groups::place_current_thread(core_groups::Role::Other, "end frame");  // (WWHD_VK_PLACE_THREADS=0: not placed)
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> l(g_mutex);
            g_cv.wait(l, [] { return g_queued; });
            j = g_job;
            g_queued = false;
            g_busy = true;
        }
        float q = 0, r = 0, s = 0;
        xs().declare_thread(vr::xr::Session::XrThread::RenderWorker);  // (Android: it calls xrEndFrame; once per session)
        end(j, q, r, s);
        note_runtime_smoothing();  // (log only; the queue's lock released)
        {
            std::lock_guard<std::mutex> l(g_mutex);
            g_queueMs = q, g_releaseMs = r, g_submitMs = s;
            g_timed = true;
            g_busy = false;
        }
        g_cv.notify_all();
    }
}

void push_times(float q, float r, float s) {
    if (g_times.submit.size() >= 4096) return;
    g_times.queue.push_back(q);
    g_times.release.push_back(r);
    g_times.submit.push_back(s);
}

// the render thread: hands the frame's end over (the thread is idle: frame_begin waited for it)
void submit(const Job& j) {
    if (!on()) {
        float q = 0, r = 0, s = 0;
        end(j, q, r, s);
        push_times(q, r, s);
        return;
    }
    static std::thread* t = new std::thread(loop);  // (lives as long as the process; it only waits between frames)
    (void)t;
    {
        std::lock_guard<std::mutex> l(g_mutex);
        g_job = j;
        g_queued = true;
    }
    g_cv.notify_all();
}

// the render thread, before the session's next frame call (xrWaitFrame, a frame shown again, the session's end):
// the last frame's end is done
void wait() {
    if (!on()) return;
    const auto start = std::chrono::steady_clock::now();
    float q = 0, r = 0, s = 0;
    bool timed = false, waited = false;
    {
        std::unique_lock<std::mutex> l(g_mutex);
        waited = g_queued || g_busy;
        g_cv.wait(l, [] { return !g_queued && !g_busy; });
        if (g_timed) q = g_queueMs, r = g_releaseMs, s = g_submitMs, timed = true, g_timed = false;
    }
    if (timed) push_times(q, r, s);
    if (waited && g_times.join.size() < 4096)
        g_times.join.push_back(float(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()));
}
}  // namespace end_thread

int64_t g_lockedVsync = -1;  // (the period lock_to_headset set; -1: none yet this session)
void restore_frame_rate() {
    interp::set_vr_display_hz(0);
    interp::set_vr_streamed(false);
    gx2::lock_vsync_to_display(0);
    g_lockedVsync = -1;
    if (g_savedInterpMode >= 0) {
        interp::set_fps(g_savedInterpFps);
        interp::set_mode(g_savedInterpMode);
        LOG("[vr] frame interpolation restored (%s)", g_savedInterpMode ? "on" : "off");
    }
    g_savedInterpMode = -1;
}

// The game's frames on the headset's clock (docs/VR_FRAME_RATE.md, "Locked to the headset's clock"): the virtual vsync
// the game steps and draws on is 1.5 refreshes of a 90 Hz headset (2 of a 120 Hz one), so its 30 Hz steps and 45 or
// 90 fps frames keep the headset's pace (30.00 steps a second instead of the console's 29.97). A rate that is a whole
// number of refreshes over 3 or 5 steps locks too (2026-11-03): 72 Hz is 12 refreshes in 5 steps (its 36 fps half rate,
// 6 frames of 2 refreshes), 80 Hz 8 in 3 (40 fps, 4 frames of 2); the vsync is then 1/60 s on the headset's clock
void lock_to_headset() {
    const double hz = xs().refresh_rate();
    int64_t period = 0;
    for (int steps : {1, 3, 5}) {
        const double inSteps = hz * steps / 30.0;  // refreshes in that many steps
        const long refreshes = std::lround(inSteps);
        if (hz > 0 && refreshes >= 2 * steps && std::fabs(inSteps - double(refreshes)) < 0.02 * steps) {
            period = std::llround(1e9 * double(refreshes) / (2.0 * steps * hz));  // (two vsyncs a step)
            break;
        }
    }
    if (period == g_lockedVsync) return;
    g_lockedVsync = period;
    gx2::lock_vsync_to_display(period);
}
// A frame shown on another refresh than the one its interval says: with the clocks locked, the frames reach the
// headset at the same point of its refresh every time, and if that point is near its deadline the timing's small
// variations put some of them on the refresh after (a frame shown for 3 refreshes, the next for 1: judder). A few of
// those within 2 s move the game's frames half a refresh later, once: away from the deadline
void note_display(XrTime shown, XrTime last) {
    // not on a streamed headset (docs/VR_STREAMING.md): its display times move with the network, so frames looked late
    // that were not, and each move was a hitch of its own (a Quest through VDXR on Wi-Fi: judder when looking around,
    // several moves a session, display times up to 155 ms apart). Its runtime paces the frames it gets
    // (only with Streaming optimal: off, every frame-rate mode keeps the wired headset's rules, this one too; asked for,
    // 2026-11-05)
    if (xs().streamed() && vr::config().streamingOptimal) return;
    const double hz = xs().refresh_rate();
    const int fps = interp::mode() == 1 ? interp::output_fps() : 30;
    if (hz <= 0 || fps <= 0 || !last || shown <= last) return;
    const double refresh = 1e9 / hz, expected = 1e9 / double(fps);
    // only while the game keeps its rate, a whole number of refreshes a frame: at 90 fps the game drew 60 (frames 1 and
    // 2 refreshes in turn), and in loads and menus it has no steady rate; every frame there looked like a slip and the
    // grid moved half a refresh every 5 s, a hitch each time (seen in a headset session's log)
    const double perFrame = expected / refresh;
    if (std::fabs(perFrame - std::round(perFrame)) > 0.05 || !(g_gameFrameInterval > 0) ||
        std::fabs(g_gameFrameInterval - expected) > 0.05 * expected)
        return;
    const double off = std::fabs(double(shown - last) - expected);
    if (off < 0.4 * refresh || off > 3.0 * refresh) return;  // (on time; or a stall, not the deadline)
    auto& t = g_times;
    const auto now = std::chrono::steady_clock::now();
    if (now - t.slipSince > std::chrono::seconds(2)) {
        t.slipSince = now;
        t.slips = 0;
    }
    if (++t.slips < 3 || now - t.lastDelay < std::chrono::seconds(5)) return;
    t.slips = 0;
    t.lastDelay = now;
    gx2::delay_vsync_grid(int64_t(refresh / 2));
    static int logged = 0;
    if (logged++ < 20) LOG("[vr] frames near the headset's deadline: moved half a refresh later (%.2f ms)", refresh / 2e6);
}

// the refresh rate chosen (vr.refreshRate) asked of the headset where the runtime lets the game choose: at the session's
// start and again when the setting changes during play (F1 > VR). Asked once per choice: the runtime's answer, the
// headset's actual rate, is what the frame rate follows (set_vr_display_hz) and what the settings overlay shows
float g_refreshAsked = 0;
void request_refresh_rate(const vr::Config& c) {
    if (c.refreshRate == g_refreshAsked) return;
    g_refreshAsked = c.refreshRate;
    if (!xs().refresh_rate_choosable()) {
        LOG("[vr] the runtime does not let the game choose the refresh rate (%.0f Hz chosen): the headset runs at %.0f Hz",
            double(c.refreshRate), double(xs().refresh_rate()));
        return;
    }
    xs().request_refresh_rate(c.refreshRate);
}
// (2026-11-13) the rate asked for, checked against the one the headset runs: a Quest 3S's runtime accepted 90 Hz and
// stayed at 72 (no change event; the frame rate then capped at half of 72). While they differ, every 2 s the rate is
// asked for again, 10 times at most per choice; logged when it takes, or when it never did
void verify_refresh_rate() {
    static float verifying = 0;
    static int tries = 0;
    static std::chrono::steady_clock::time_point last;
    if (!(g_refreshAsked > 0) || !xs().refresh_rate_choosable()) return;
    if (verifying != g_refreshAsked) verifying = g_refreshAsked, tries = 0, last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(2)) return;
    last = now;
    const float actual = xs().refresh_rate();
    if (std::fabs(actual - g_refreshAsked) < 0.6f) {
        if (tries > 0) {
            LOG("[vr] the headset now runs at %.0f Hz, as chosen (after asking %d more time%s)", double(actual), tries,
                tries == 1 ? "" : "s");
            tries = -1;  // (done for this choice)
        }
        return;
    }
    if (tries < 0 || tries >= 10) {
        if (tries == 10) {
            LOG("[vr] the headset still runs at %.0f Hz although %.0f Hz was asked for 11 times: its own setting (Settings > "
                "Display, or Battery Saver) may hold it there", double(actual), double(g_refreshAsked));
            tries = 11;
        }
        return;
    }
    ++tries;
    LOG("[vr] the headset runs at %.0f Hz, not the %.0f Hz chosen: asked again (%d of 10)", double(actual),
        double(g_refreshAsked), tries);
    xs().request_refresh_rate(g_refreshAsked);
}

// the session runs: the headset's refresh rate, and frame interpolation for it
void session_started() {
    g_refreshAsked = 0;
    dynres::watch_gpu(true);  // ([vr] GPU busy, every 10 s)
    const auto c = vr::config();
    request_refresh_rate(c);
    if (c.autoFrameRate && interp::mode() == 0) {
        // 30 fps in a headset is uncomfortable: interpolation for the session (the game logic stays at 30 Hz)
        g_savedInterpMode = interp::mode();
        g_savedInterpFps = interp::fps();
        interp::set_fps(120);
        interp::set_mode(1);
        LOG("[vr] frame interpolation on for the VR session (vr.autoFrameRate; not saved)");
    }
    g_started = true;
}

void log_stats() {
    const auto now = std::chrono::steady_clock::now();
    const double s = std::chrono::duration<double>(now - g_stats.since).count();
    if (s < 10.0) return;
    const uint64_t steps = interp::logic_steps();
    LOG("[vr] timing: %.1f frames/s to the headset (%.1f begun), headset %.1f Hz, xrWaitFrame %.2f ms per frame, "
        "game %.1f frames/s, logic %.1f steps/s (30 expected), interpolation %d fps; %llu frames shown again while the "
        "game had none ready (headset_frame); %llu with the last views (no head pose)",
        g_stats.submitted / s, g_stats.begun / s, double(xs().refresh_rate()),
        g_stats.begun ? g_stats.waitMs / double(g_stats.begun) : 0.0, (R.frame - g_stats.renderFrame) / s,
        (steps - g_stats.logicSteps) / s, interp::mode() == 1 ? interp::output_fps() : 30,
        (unsigned long long)g_stats.keptAlive, (unsigned long long)g_stats.viewsReused);
    vr::Fov f;
    const bool fov = vr::projection(f);
    const auto e = vr::eye_offsets();
    const float scale = vr::world_scale();
    LOG("[vr] eyes: offsets %.2f / %.2f game units (%.1f mm apart), field of view %.1f x %.1f degrees%s, images "
        "%ux%u per eye from %ux%u pictures, %.0f%% of frames with the game camera's views",
        double(e.x[0]), double(e.x[1]), double((e.x[1] - e.x[0]) / scale * 1000.0f),
        fov ? double(2 * std::atan(std::tan(f.right)) * 180 / vr::kPi) : 0.0, fov ? double(2 * f.up * 180 / vr::kPi) : 0.0,
        fov ? "" : " (not set)", xs().swapchain(0).width, xs().swapchain(0).height,
        R.tv.scan ? R.tv.scan->extent.width : 0, R.tv.scan ? R.tv.scan->extent.height : 0,
        g_stats.submitted ? 100.0 * g_stats.withViews / double(g_stats.submitted) : 0.0);
    // (diagnostics, WWHD_VR_FOV_DIAG=1: docs/VR_EYE_PROJECTION.md) per eye: the runtime's field of view, what the game
    // draws and submits, and the projection its draws actually got, with the field that projection covers in the part
    // of the image submitted beside the field submitted for it (they must be equal: else the picture is magnified or
    // shrunk in the headset, the classic cause of a "bubble" or "zoomed" look)
    if (fov_diag::kOn) {
        const auto deg = [](float r) { return double(r) * 180.0 / vr::kPi; };
        vr::xr::Views raw;
        const bool located = xs().locate_views(xs().predicted_display_time(), raw);
        LOG("[vr fov] mode: %s; eye crop %s; margin %.1f deg; recommended image %ux%u, swapchain %ux%u; eye picture %ux%u "
            "(render targets x%.4f wide, x%.4f tall of 16:9)",
            vr::own_eye_projection() ? "each eye its own projection" : "one symmetric projection, each eye submitted as its part",
            vr::config().eyeCrop == vr::EyeCrop::Off ? "off" : vr::config().eyeCrop == vr::EyeCrop::On ? "on" : "auto",
            deg(vr::fov_margin()), xs().recommended_width(), xs().recommended_height(), xs().swapchain(0).width,
            xs().swapchain(0).height, R.tv.scan ? R.tv.scan->extent.width : 0, R.tv.scan ? R.tv.scan->extent.height : 0,
            double(aspect::requested() >= 16.0f / 9.0f ? aspect::requested() / (16.0f / 9.0f) : 1.0f),
            double(aspect::requested() >= 16.0f / 9.0f ? 1.0f : (16.0f / 9.0f) / aspect::requested()));
        for (int e = 0; e < 2; ++e) {
            const char* name = e ? "right" : "left";
            if (located) {
                const vr::Fov& r = raw.eye[e].fov;
                LOG("[vr fov] %s eye, runtime: left %.2f right %.2f up %.2f down %.2f deg (%.4f %.4f %.4f %.4f rad); %.2f x "
                    "%.2f deg; at %.1f %.1f %.1f mm from the head",
                    name, deg(r.left), deg(r.right), deg(r.up), deg(r.down), double(r.left), double(r.right), double(r.up),
                    double(r.down), deg(r.right - r.left), deg(r.up - r.down),
                    double(raw.eye[e].pose.position.x - raw.head.position.x) * 1000.0,
                    double(raw.eye[e].pose.position.y - raw.head.position.y) * 1000.0,
                    double(raw.eye[e].pose.position.z - raw.head.position.z) * 1000.0);
            }
            const stereo::EyeFrustum& fr = stereo::frame.frustum[e];
            const auto sub = xs().submitted(e);
            const auto& sc = xs().swapchain(e);
            if (sub.valid && sc.width && sc.height) {
                const double kept = double(sub.width) * sub.height / (double(sc.width) * sc.height);
                const double pxAcross = sub.width / (std::tan(double(sub.fov.right)) - std::tan(double(sub.fov.left))) * vr::kPi / 180.0;
                const double pxDown = sub.height / (std::tan(double(sub.fov.up)) - std::tan(double(sub.fov.down))) * vr::kPi / 180.0;
                LOG("[vr fov] %s eye, submitted: left %.2f right %.2f up %.2f down %.2f deg; pixels %d..%d x %d..%d of %ux%u "
                    "(%.1f%% of the image discarded); %.1f x %.1f pixels per degree at the centre; frustum sx %.4f ox %.4f "
                    "sy %.4f oy %.4f",
                    name, deg(sub.fov.left), deg(sub.fov.right), deg(sub.fov.up), deg(sub.fov.down), sub.x,
                    sub.x + sub.width, sub.y, sub.y + sub.height, sc.width, sc.height, 100.0 * (1.0 - kept), pxAcross, pxDown,
                    double(fr.sx), double(fr.ox), double(fr.sy), double(fr.oy));
            }
            fov_diag::Seen& s = fov_diag::g_seen[e];
            const unsigned n = s.count.exchange(0, std::memory_order_relaxed);
            if (!n || !sub.valid || !sc.width || !sc.height) {
                LOG("[vr fov] %s eye, drawn: no projection seen in its draws this time", name);
                continue;
            }
            float l, rt, u, d;
            fov_diag::field(s.p0.load(), s.p2.load(), s.p5.load(), s.p6.load(), l, rt, u, d);
            // the field those draws put in the submitted part of the image (the image's whole field: the draws')
            const auto at = [](float a, float b, double t) { return std::atan(std::tan(double(a)) + (std::tan(double(b)) - std::tan(double(a))) * t); };
            const double pl = at(l, rt, double(sub.x) / sc.width), pr = at(l, rt, double(sub.x + sub.width) / sc.width);
            const double pu = at(u, d, double(sub.y) / sc.height), pd = at(u, d, double(sub.y + sub.height) / sc.height);
            const double off = std::max({std::fabs(pl - sub.fov.left), std::fabs(pr - sub.fov.right), std::fabs(pu - sub.fov.up),
                                         std::fabs(pd - sub.fov.down)}) * 180.0 / vr::kPi;
            LOG("[vr fov] %s eye, drawn: projection p0 %.5f p2 %.5f p5 %.5f p6 %.5f (%u draws), the image left %.2f right %.2f "
                "up %.2f down %.2f deg; in the submitted part left %.2f right %.2f up %.2f down %.2f deg: %s (at most %.3f "
                "deg off)",
                name, double(s.p0.load()), double(s.p2.load()), double(s.p5.load()), double(s.p6.load()), n, deg(l), deg(rt),
                deg(u), deg(d), pl * 180.0 / vr::kPi, pr * 180.0 / vr::kPi, pu * 180.0 / vr::kPi, pd * 180.0 / vr::kPi,
                off < 0.05 ? "MATCHES the submitted field" : "DOES NOT MATCH the submitted field", off);
        }
        // the layouts (HUD, menus, text) in each eye's picture and how they got the eye's frustum (fov_diag.h)
        for (int e = 0; e < 2; ++e) {
            fov_diag::LayoutStats& l = fov_diag::g_layout[e];
            std::string top;
            {
                std::lock_guard<std::mutex> lock(fov_diag::g_layoutMutex);
                std::vector<std::pair<unsigned, uint32_t>> v;
                for (auto& [vs, n] : fov_diag::g_unpatched[e]) v.push_back({n, vs});
                fov_diag::g_unpatched[e].clear();
                std::sort(v.rbegin(), v.rend());
                for (size_t i = 0; i < v.size() && i < 6; ++i) {
                    char b[40];
                    std::snprintf(b, sizeof b, " VS %08X x%u", v[i].second, v[i].first);
                    top += b;
                }
            }
            LOG("[vr fov] %s eye, layouts drawn into it: %u; given the eye's frustum by the packer %u, a uniform block %u, a "
                "batch %u, eye 1's replay %u; given none %u%s%s",
                e ? "right" : "left", l.draws.exchange(0), l.pack.exchange(0), l.block.exchange(0), l.batch.exchange(0),
                l.replay.exchange(0), l.none.exchange(0), top.empty() ? "" : ":", top.c_str());
        }
    }
    // frame times: percentiles, not averages (a few long frames are the stutter an average hides)
    {
        auto& t = g_times;
        const double hz = xs().refresh_rate();
        const float budget = hz > 0 ? float(1000.0 / hz) : 11.11f;
        int missed = 0, repeats = 0, late = 0;
        for (float d : t.display) {
            missed += d > 1.5f;
            repeats += d > 1.5f ? int(std::lround(d)) - 1 : 0;
        }
        for (float i : t.interval) late += i > budget * 1.5f;
        // even motion: each frame shown moves the game on by 30/fps steps (the frames drawn a second); a frame off by
        // more than a third of that is uneven (judder), whatever the frame rate
        const int fps = interp::mode() == 1 ? interp::output_fps() : 30;
        const float even = fps > 0 ? float(30.0 / fps) : 1.0f / 3.0f;
        int uneven = 0;
        for (float g : t.game) uneven += std::fabs(g - even) > even / 3;
        const size_t shown = t.cpu.size();
        LOG("[vr] frame times over %zu frames (budget %.2f ms): interval p50 %.2f / p95 %.2f / p99 %.2f / max %.2f ms, "
            "%d over 1.5x; render thread after xrWaitFrame p50 %.2f / p99 %.2f ms; xrWaitFrame p50 %.2f / p99 %.2f ms; "
            "missed display periods %d (in %d frames); game time per frame p50 %.3f / p5 %.3f / p95 %.3f steps "
            "(even: %.3f), %d of %zu frames uneven",
            shown, double(budget), double(percentile(t.interval, 0.5f)), double(percentile(t.interval, 0.95f)),
            double(percentile(t.interval, 0.99f)), double(percentile(t.interval, 1.0f)), late,
            double(percentile(t.cpu, 0.5f)), double(percentile(t.cpu, 0.99f)), double(percentile(t.wait, 0.5f)),
            double(percentile(t.wait, 0.99f)), repeats, missed, double(percentile(t.game, 0.5f)),
            double(percentile(t.game, 0.05f)), double(percentile(t.game, 0.95f)), double(even), uneven, t.game.size());
        // (docs/VR_STREAMING.md, the wireless audit of 2026-11-05) frame pacing as frame rates: the average, the 1 % and
        // 0.1 % lows (the 99th and 99.9th percentile intervals) and the intervals' standard deviation. Frames sent
        // unevenly are what a streamed headset shows worst; the game's own pacing apart from the network's
        if (!t.interval.empty()) {
            double sum = 0, sq = 0;
            for (float i : t.interval) {
                sum += i;
                sq += double(i) * i;
            }
            const double n = double(t.interval.size()), mean = sum / n;
            const double sd = std::sqrt(std::max(0.0, sq / n - mean * mean));
            const double p99 = percentile(t.interval, 0.99f), p999 = percentile(t.interval, 0.999f);
            LOG("[vr] frame pacing: %.1f fps on average, 1%% low %.1f fps, 0.1%% low %.1f fps; intervals %.2f ms "
                "+- %.2f ms (the headset's refresh %.2f ms)",
                mean > 0 ? 1000.0 / mean : 0.0, p99 > 0 ? 1000.0 / p99 : 0.0, p999 > 0 ? 1000.0 / p999 : 0.0, mean, sd,
                double(budget));
        }
        if (!t.submit.empty()) {
            LOG("[vr] submission per frame: eye images acquired p50 %.2f / p99 %.2f ms; graphics queue taken p50 %.2f / "
                "p99 %.2f ms; released + xrEndFrame p50 %.2f / p99 %.2f ms (released alone p50 %.2f / p99 %.2f ms)",
                double(percentile(t.acquire, 0.5f)), double(percentile(t.acquire, 0.99f)), double(percentile(t.queue, 0.5f)),
                double(percentile(t.queue, 0.99f)), double(percentile(t.submit, 0.5f)), double(percentile(t.submit, 0.99f)),
                double(percentile(t.release, 0.5f)), double(percentile(t.release, 0.99f)));
            // (the end-frame thread) how long the render thread still waited for it at the next frame_begin
            if (end_thread::on())
                LOG("[vr] end-frame thread: the render thread waited for it in %zu of %zu frames, p50 %.2f / p99 %.2f / max "
                    "%.2f ms",
                    t.join.size(), t.submit.size(), double(percentile(t.join, 0.5f)), double(percentile(t.join, 0.99f)),
                    double(percentile(t.join, 1.0f)));
        }
        // the GPU's busy time (dynres::watch_gpu: every command buffer timed on the GPU, overlaps counted once), per
        // frame shown and as a share of the 10 s: a streamed headset's video encoder runs on the same GPU
        // (docs/VR_STREAMING.md), and a GPU near full leaves it nothing
        {
            const double busyNs = dynres::take_watched_ns();
            if (busyNs > 0)
                LOG("[vr] GPU busy: %.0f%% of the time, %.2f ms per frame shown%s", 100.0 * busyNs / (s * 1e9),
                    shown ? busyNs * 1e-6 / double(shown) : 0.0,
                    xs().streamed() ? " (a streamed headset: its video encoder shares the GPU)" : "");
        }
        // the VR work on the game thread, per frame shown (vr.h GameWork)
        uint64_t work[int(vr::GameWork::Count)];
        vr::take_game_work(work);
        const double perFrame = shown ? 1e-6 / double(shown) : 0.0;
        double total = 0;
        for (uint64_t w : work) total += double(w);
        LOG("[vr] VR work on the game thread: %.2f ms per frame shown (camera %.2f, pad %.2f, Link's execute %.2f, "
            "arms %.2f, draw %.2f)",
            total * perFrame, double(work[int(vr::GameWork::Camera)]) * perFrame, double(work[int(vr::GameWork::Pad)]) * perFrame,
            double(work[int(vr::GameWork::Execute)]) * perFrame, double(work[int(vr::GameWork::Arms)]) * perFrame,
            double(work[int(vr::GameWork::Draw)]) * perFrame);
        // the camera's pose prediction: a frame's change in it moves the predicted display time by a whole frame
        uint64_t ahead[5], aheadChanges = 0;
        vr::game::take_camera_ahead(ahead, aheadChanges);
        LOG("[vr] camera pose predicted ahead (frames: camera frames): 1: %llu, 2: %llu, 3: %llu, 4: %llu (0: %llu); "
            "changed from the frame before %llu times",
            (unsigned long long)ahead[1], (unsigned long long)ahead[2], (unsigned long long)ahead[3],
            (unsigned long long)ahead[4], (unsigned long long)ahead[0], (unsigned long long)aheadChanges);
        if (dynres::on()) LOG("[vr] %s", dynres::status().c_str());  // (docs/VR_DYNAMIC_RESOLUTION.md)
#if defined(__linux__)
        // (QUEST3_PERFORMANCE_AUDIT.md, memory) Android and Linux: the game's resident memory and its peak, and what
        // the system still has free; a Quest 3 gives an app about 5.75 GiB, and Android ends an app that runs it short
        {
            auto field = [](const char* path, const char* key) -> long long {
                FILE* f = std::fopen(path, "r");
                if (!f) return -1;
                char line[256];
                long long v = -1;
                const size_t n = std::strlen(key);
                while (std::fgets(line, sizeof line, f))
                    if (!std::strncmp(line, key, n)) { v = std::atoll(line + n); break; }
                std::fclose(f);
                return v;  // (kB)
            };
            const long long rss = field("/proc/self/status", "VmRSS:"), peak = field("/proc/self/status", "VmHWM:");
            const long long avail = field("/proc/meminfo", "MemAvailable:"), total = field("/proc/meminfo", "MemTotal:");
            if (rss >= 0)
                LOG("[vr] memory: the game %.0f MiB resident (peak %.0f MiB); the system %.0f of %.0f MiB available",
                    double(rss) / 1024.0, double(peak) / 1024.0, double(avail) / 1024.0, double(total) / 1024.0);
        }
#endif
        t.clear();
    }
    g_stats = {};
    g_stats.renderFrame = R.frame;
    g_stats.logicSteps = steps;
}

// (docs/VR_PERFORMANCE.md, "PC VR at 3x by default") PC VR draws at the 3x internal resolution unless the player chose
// one (resScale in settings.ini, or WWHD_RES_SCALE): the desktop's default 1x drew 1230x1335 eye pictures for a Quest 3's
// 2752x2940 (a Virtual Desktop player's log, the RTX 5080 busy 5 %). Standalone headsets keep their own. The desktop's
// resolution comes back when the session ends, unless the player saved one meanwhile (F1 > Graphics)
#if !(defined(__ANDROID__) || defined(__aarch64__))
constexpr float kPcVrResScale = 3.0f;
bool g_resDefaultChecked = false;
float g_resBeforeVr = 0;  // the resolution before this session's default (0: none applied)
void apply_vr_res_default() {
    if (g_resDefaultChecked) return;
    g_resDefaultChecked = true;
    std::string saved;
    if (hostui::get("resScale", saved) || std::getenv("WWHD_RES_SCALE")) return;
    g_resBeforeVr = requested_res_scale();
    if (g_resBeforeVr == kPcVrResScale) return;
    LOG("[vr] internal resolution %gx for PC VR (no resScale saved; F1 > Graphics changes it)", double(kPcVrResScale));
    set_res_scale(kPcVrResScale);
}
void restore_res_after_vr() {
    g_resDefaultChecked = false;
    const float before = g_resBeforeVr;
    g_resBeforeVr = 0;
    std::string saved;
    if (before > 0 && before != kPcVrResScale && !hostui::get("resScale", saved)) set_res_scale(before);
}
#else
void apply_vr_res_default() {}
void restore_res_after_vr() {}
#endif
void stop(const std::string& why) {
    end_thread::wait();
    restore_res_after_vr();
    dynres::watch_gpu(false);
    if (g_frame) {
        xs().end_frame({}, false);
        g_frame = false;
    }
    stereo::set_xr(false);
    stereo::startingXR = false;
    dynres::set_vr(false, false, 1, 1, 0, 0);  // (the Graphics setting again)
    motion_pass::set_needed(true);  // (the flat game's DLSS: as before)
    vr::dynres_vr::status_why().store(nullptr, std::memory_order_relaxed);
    vr::set_active(false);
    vr::set_projection({}, false);
    // (docs/VR_EYE_PROJECTION.md) the eyes' own shape and frustums end with the session: the desktop picture after VR
    // kept the narrower eye shape (render targets x0.59 wide)
    vr::set_eye_image_aspect(0);
    vr::set_picture_size(0, 0);  // (the desktop's internal resolution again)
    stereo::set_eye_frustums({}, {});
    gfx::display_vr_wants_gamepad_state(false);
    restore_frame_rate();
    g_started = false;
    g_shown.valid = false;
    g_panelShown = false;
    if (R.device) {
        // the session's swapchain images must be idle before the runtime destroys them
        std::lock_guard<std::mutex> queueLock(graphics_queue_mutex());
        const auto uploadQueueLock = texture_pack_queue_lock();  // (the texture packs' upload queue: idle too)
        vkDeviceWaitIdle(R.device);
        destroy_panel_views();
        vr_upscale::forget_views();
        vr_cas::forget_views();
        vr_sgsr2::forget();
        vr_fsr1::forget();
        vr_framesynth::forget_views();
        vr_genframe::forget_views();
        vr_framesynth::set_active(false);
    }
    xs().shutdown();
    vr::set_status(why);
}
}  // namespace

VkResult create_instance(const VkInstanceCreateInfo& ci, VkInstance* out) {
    if (!g_tried) vr::load_settings();  // vr.enabled decides (with --vr / --no-vr / WWHD_VR) whether to try
    if (!g_tried && vr::wanted()) {
        g_tried = true;
        // (STEAM_FRAME_VR_OPTIMIZATION_AUDIT.md, "Valve's layers") SteamOS's launch wrapper on the Steam Frame adds its own
        // Vulkan layers to every game through VK_INSTANCE_LAYERS (VK_LAYER_VALVE_rpo, VK_LAYER_VALVE_fdm_injection, the
        // latter driven by Steam's per-game Foveated Rendering switch and FDM_DEBUG): logged, so a headset log says whether
        // they were in the process. Nothing is changed
        {
            const char* layers = std::getenv("VK_INSTANCE_LAYERS");
            const char* disabled = std::getenv("VK_LOADER_LAYERS_DISABLE");
            const char* fdm = std::getenv("FDM_DEBUG");
            if ((layers && *layers) || (disabled && *disabled) || (fdm && *fdm))
                LOG("[vr] Vulkan layers from the environment: VK_INSTANCE_LAYERS=%s; VK_LOADER_LAYERS_DISABLE=%s; FDM_DEBUG=%s",
                    layers && *layers ? layers : "(unset)", disabled && *disabled ? disabled : "(unset)",
                    fdm && *fdm ? fdm : "(unset)");
        }
        if (!xs().init("The Wind Waker HD")) {
            vr::set_status("VR is not available: " + xs().why() + ". Playing on the desktop.");
        } else {
            const auto req = xs().requirements();
            if (ci.pApplicationInfo && req.maxApi && VK_API_VERSION_MAJOR(req.minApi) > 1)
                LOG("[vr] the runtime asks for Vulkan %u.%u or newer", VK_API_VERSION_MAJOR(req.minApi),
                    VK_API_VERSION_MINOR(req.minApi));
            const VkResult r = xs().create_vulkan_instance(ci, vkGetInstanceProcAddr, out);
            if (r == VK_SUCCESS) {
                g_xrInstance = true;
                stereo::startingXR = true;
                g_instance = *out;
                LOG("[vr] Vulkan instance created through the OpenXR runtime");
                return r;
            }
            LOG("[vr] the OpenXR runtime could not create the Vulkan instance (%d)", int(r));
            stop("VR is not available: the OpenXR runtime could not create a Vulkan instance. Playing on the desktop.");
        }
    }
    return vkCreateInstance(&ci, nullptr, out);
}

int preferred_device(const std::vector<VkPhysicalDevice>& devices) {
    if (!g_xrInstance) return -1;
    g_physical = xs().vulkan_physical_device(g_instance);
    for (size_t i = 0; i < devices.size(); ++i)
        if (devices[i] == g_physical) return int(i);
    if (g_physical) LOG("[vr] the headset's GPU is not among the usable devices");
    stop("VR is not available: the headset is connected to a GPU the game cannot use. Playing on the desktop.");
    return -1;
}

VkResult create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo& ci, VkDevice* out) {
    if (g_xrInstance && xs().initialised() && physical == g_physical) {
        const VkResult r = xs().create_vulkan_device(physical, ci, vkGetInstanceProcAddr, out);
        if (r == VK_SUCCESS) {
            LOG("[vr] Vulkan device created through the OpenXR runtime");
            return r;
        }
        stop("VR is not available: the OpenXR runtime could not create the Vulkan device. Playing on the desktop.");
    }
    return vkCreateDevice(physical, &ci, nullptr, out);
}

void device_ready() {
    if (!xs().initialised()) return;
    // (docs/VR_SETTINGS.md, "Headset resolution") vr.renderScale: the eye pictures at that share of the runtime's
    // recommended size. On a PC the headset's images are that size too (a 1:1 copy); a standalone headset keeps the
    // runtime's own size for them and upscales the smaller pictures into them (Quest Super Resolution, SGSR1)
    const float headsetRes = vr::config().renderScale;
    if (!xs().begin(R.instance, R.physicalDevice, R.device, R.queueFamily, 0, vr::kPcVrBuild ? headsetRes : 1.0f)) {
        stop("VR is not available: " + xs().why() + ". Playing on the desktop.");
        return;
    }
    {
        // (docs/VR_EYE_PROJECTION.md "Field of view") a narrower field (vr.fieldOfView) draws a picture smaller by as
        // much: the same pixels per degree
        vr::latch_fov_scale();
        const float fovScale = vr::fov_scale();
        const uint32_t w = std::max(64u, uint32_t(std::lround(float(xs().recommended_width()) * headsetRes * fovScale)));
        const uint32_t h = std::max(64u, uint32_t(std::lround(float(xs().recommended_height()) * headsetRes * fovScale)));
        vr::set_picture_size(w, h);
        LOG("[vr] headset resolution %.0f%% of the runtime's %ux%u: eye pictures %ux%u%s", double(headsetRes * 100),
            xs().recommended_width(), xs().recommended_height(), w, h,
            vr::kPcVrBuild ? " (the headset's images the same size)" : " (upscaled into its images)");
    }
    vr::set_status("");
    LOG("[vr] session created; waiting for the runtime to start it");
}

void frame_begin() {
    end_thread::wait();  // (the last frame's xrEndFrame before this one's xrWaitFrame, and before any session event)
    g_recorded = false;
    g_frameViews = vr::render_views();
    if (!xs().initialised()) return;
    xs().poll_events();
    if (xs().state() == vr::xr::State::Lost) {
        stop("The VR session ended (headset or runtime). Playing on the desktop.");
        return;
    }
    const bool running = xs().running();
    if (!running) {
        stereo::set_xr(false);
        vr::set_active(false);
        return;
    }
    if (!g_started) session_started();
    xs().declare_thread(vr::xr::Session::XrThread::RenderMain);  // (Android: once per session; QUEST3_PERFORMANCE_AUDIT.md)
    const auto waitStart = std::chrono::steady_clock::now();
    g_frame = xs().begin_frame(g_shouldRender);
    // (the camera's base: display_time; with the swap being executed now, render::swap from OP_SWAP: executed + 1)
    if (g_frame) vr::set_real_frame_display(int64_t(xs().predicted_display_time()), gx2::swaps_executed() + 1);
    g_times.begun = std::chrono::steady_clock::now();
    const double waited = std::chrono::duration<double, std::milli>(g_times.begun - waitStart).count();
    g_stats.waitMs += waited;
    if (g_frame && g_times.wait.size() < 4096) g_times.wait.push_back(float(waited));
    g_stats.begun += g_frame;
    // the headset's rate is the display interpolation plans for (not the desktop monitor's)
    interp::set_vr_display_hz(int(std::lround(xs().refresh_rate())));
    interp::set_vr_streamed(xs().streamed());
    lock_to_headset();
    {
        const vr::Config c = vr::config();
        // (docs/VR_STREAMING.md) streaming optimal: a frame every refresh at 72 and 80 Hz, on a streamed headset only
        interp::set_vr_stream_rates(xs().streamed() && c.streamingOptimal);
        // (tests, VR fork) WWHD_VR_FRAME_RATE=full|half|auto|unlocked: that frame rate for this start, without changing
        // the saved vr.frameRate (headset test runs are compared at full, Capped 90)
        static const int testRate = [] {
            const char* e = std::getenv("WWHD_VR_FRAME_RATE");
            if (!e || !*e) return -1;
            const int r = !std::strcmp(e, "full") ? int(vr::FrameRate::Full) : !std::strcmp(e, "half") ? int(vr::FrameRate::Half)
                        : !std::strcmp(e, "auto") ? int(vr::FrameRate::Auto) : !std::strcmp(e, "unlocked") ? int(vr::FrameRate::Unlocked) : -1;
            LOG("[vr] WWHD_VR_FRAME_RATE=%s: %s for this start (the saved vr.frameRate is kept)", e, r < 0 ? "not a frame rate, ignored" : e);
            return r;
        }();
        // frame synthesis (docs/VR_FRAME_SYNTHESIS.md): the runtime makes every other frame, and paces the game at half
        // the refresh rate itself; the game draws at half the rate to match (a test rate still wins)
        // (2026-11-13) held while the frame rate chosen is under half the refresh rate: Capped 30 at a refresh that 30
        // divides (90, 120 Hz; at 72 and 80 Hz Capped 30 is already drawn at half, 36 and 40). The runtime synthesizes only
        // at half the refresh rate, which raised a chosen 30 to 45 (vr_framesynth::set_held)
        {
            const double hz = xs().refresh_rate();
            const double thirds = hz / 30.0;
            const bool thirtyFits = hz > 61.0 && std::fabs(thirds - std::round(thirds)) < 0.02;
            // (and Auto Mode while the game runs under half the refresh rate: update_auto_synth_hold, after each game frame)
            const bool steady30 = c.frameRate == vr::FrameRate::Original && thirtyFits;
            const bool autoSlow = c.frameRate == vr::FrameRate::Auto && g_autoSynthHeld.load(std::memory_order_relaxed);
            vr_framesynth::set_held(xs().has_synthesis_swapchains() && (steady30 || autoSlow),
                                    steady30 ? "the frame rate chosen is under half the refresh rate (Capped 30 at 90 or 120 Hz)"
                                    : autoSlow ? "Auto Mode, the game under half the refresh rate (below 36 fps at 90 Hz) for 1 s"
                                               : "the frame rate is half the refresh rate or more");
        }
        const bool synthesis = xs().has_synthesis_swapchains() && !vr_framesynth::held();
        // (and for the generated frames and Head movement smoothing: each eye's depth found each frame, vr_framesynth::last_depth)
        // (and Turn smoothing: its frames hold the near field by the scene's depth, show_generated(true))
        // (only in Capped 45 and Steady 30, where it runs: nothing added in Capped 90, Auto or Unlocked)
        const bool lowRate = c.frameRate == vr::FrameRate::Half || c.frameRate == vr::FrameRate::Original ||
                             (c.frameRate == vr::FrameRate::Auto && low_rate_repeats(xs().refresh_rate(), 0) > 0);
        vr_framesynth::set_active(synthesis || xs().depth_submission() || vr::config().headSmoothing ||
                                  (vr::config().turnSmoothing && lowRate && std::getenv("WWHD_VR_TURN_NEAR_HOLD") &&
                                   *std::getenv("WWHD_VR_TURN_NEAR_HOLD") == '1'));
        // (only while frames carry the motion vectors: menus and loading screens have none, and keep vr.frameRate)
        interp::set_vr_frame_mode(testRate >= 0 ? testRate
                                  : synthesis && vr_framesynth::carrying() ? int(vr::FrameRate::Half)
                                                                           : int(c.frameRate));  // (from the next logic step)
        if (g_started) request_refresh_rate(c);       // (vr.refreshRate changed in the game's settings)
        if (g_started) verify_refresh_rate();          // (the headset's own rate, until it is the one chosen)
    }
    // the headset's eyes for the frames drawn from now on: the field of view the game's camera and culling
    // take (aspect.cpp) and each eye's sideways offset (stereo::XR), in game units
    vr::xr::Views v;
    if (g_frame && xs().locate_views(xs().predicted_display_time(), v)) {
        float eyeX[2];
        const vr::xr::Views r = vr::as_rendered(v, eyeX);
        const float scale = vr::world_scale();
        vr::set_eye_offsets({{eyeX[0] * scale, eyeX[1] * scale}});
        // the game's camera and culling: the symmetric field both eyes' fields fit in
        const vr::Fov drawn = vr::drawn_fov(v);
        vr::set_projection(drawn, true);
        // (docs/VR_EYE_PROJECTION.md) each eye drawn with its own field of view: its frustum within the camera's, and the
        // eye pictures shaped like the eyes' fields (both the same size: the mean shape; each eye's frustum fills it)
        stereo::EyeFrustum frustum[2];
        float aspect = 0;
        g_drawnFov = drawn;
        g_eyeFovs = vr::own_eye_projection();
        if (vr::own_eye_projection()) {
            for (int e = 0; e < 2; ++e) {
                const vr::EyeFrustumXY f = vr::eye_frustum(drawn, r.eye[e].fov);
                frustum[e] = {f.sx, f.ox, f.sy, f.oy};
                aspect += 0.5f * vr::fov_image_aspect(r.eye[e].fov);
                g_eyeFov[e] = r.eye[e].fov;
            }
        }
        vr::set_eye_fields(r.eye[0].fov, r.eye[1].fov, g_eyeFovs);
        stereo::set_eye_frustums(frustum[0], frustum[1]);
        vr::set_eye_image_aspect(aspect);
    }
    const auto eyes = vr::eye_offsets();
    stereo::set_xr(true, eyes.x[0], eyes.x[1]);
    vr::set_active(true);
    apply_vr_res_default();  // (once a session: PC VR at 3x unless the player chose a resolution)
    // the GamePad screen as a panel (pause, item, map, bag menus): while its menus are open (or always), at the
    // menu settings' distance, height and size (vr.menu*); placed where the head looks when it appears and then
    // fixed in the room (gaze), following the head (head), or ahead of the tracking space's forward (front)
    const auto c = vr::config();
    // (the VR menu, docs/VR_MENU.md: the settings shown on the same panel while it is open, whatever the GamePad's; and
    // the new save's name: its keyboard, the overlay's text prompt, the same way)
    const bool menu = vr::game::menu_open() || vr::game::name_entry_open();
    if (menu != g_panelMenu) {  // (placed again, where you look, for what it shows now)
        g_panelShown = false;
        g_panelMenu = menu;
    }
    const bool want = menu || c.gamepadScreen == vr::GamePadScreen::Always ||
                      (c.gamepadScreen == vr::GamePadScreen::Menus && gfx::display_vr_gamepad_panel_open());
    if (want && g_frame && v.valid && (!g_panelShown || c.menuPlacement == vr::MenuPlacement::Head)) {
        const float aspect = menu ? float(kMenuPanelHeight) / float(kMenuPanelWidth)
                             : R.drc.scan ? float(R.drc.scan->extent.height) / float(std::max(1u, R.drc.scan->extent.width)) : 480.0f / 854.0f;
        vr::Pose from = v.head;
        if (c.menuPlacement == vr::MenuPlacement::Front) from.orientation = {};
        g_panel.pose = vr::panel_pose(from, c.menuDistance, -c.menuHeight);
        g_panel.width = vr::kMenuBaseWidth * c.menuScale * (menu ? kMenuPanelScale : 1.0f);
        g_panel.height = g_panel.width * aspect;
        g_panel.curvature = c.menuCurvature;
        g_panel.distance = c.menuDistance;
        if (!g_panelShown)
            LOG("[vr] GamePad screen shown %.2f m ahead, %.2f m wide (%s)", double(c.menuDistance), double(g_panel.width),
                c.menuPlacement == vr::MenuPlacement::Gaze ? "where you look" : c.menuPlacement == vr::MenuPlacement::Head ? "following the head" : "ahead");
    }
    g_panelShown = want && (g_panelShown || v.valid);
    gfx::display_vr_gamepad_unseen(!want && skip_desktop_gamepad());
}

// a quad's swapchain made (or remade at a new size) mid-session: under the graphics queue's lock, as release and end
// frame are. The runtime may use the queue in xrCreateSwapchain and xrDestroySwapchain, while the end-frame thread and
// the record worker submit to it (Quest 3 through VDXR on an RTX 5080, 2026-11-03: a crash in NVIDIA's driver the frame
// the UI layer's swapchain was made). Already made at that size: no lock (the end-frame thread holds it for ms)
template <class Ensure>
bool ensure_swapchain(int index, uint32_t width, uint32_t height, Ensure&& ensure) {
    const auto& sc = xs().swapchain(index);
    const bool made = sc.handle && (index == vr::xr::Session::kUi || (sc.width == width && sc.height == height));
    if (made) return ensure();
    // (2026-11-12) a swapchain made again at another size (the GamePad panel: 2562x1440 for the GamePad screen,
    // 1600x900 for the VR menu) destroys its images: first the last frame's end (the end-frame thread's xrEndFrame
    // names them), then everything the GPU was given (earlier frames drew into them), then the cached views of the old
    // images (a new image may come back with an old one's handle). Destroyed while still in use, a PC player's RTX 4090
    // lost its device as the menu opened a second after it closed (DEVICE_LOST, two crash logs, 2026-11-12)
    const bool remade = sc.handle && sc.acquired == UINT32_MAX;
    if (remade) end_thread::wait();
    std::lock_guard<std::mutex> queueLock(graphics_queue_mutex());
    if (remade && R.device) {
        const auto uploadQueueLock = texture_pack_queue_lock();
        vkDeviceWaitIdle(R.device);
        if (index == vr::xr::Session::kPanel) destroy_panel_views();
        LOG("[vr] swapchain %d made again at %ux%u (was %ux%u): the old images idle first", index, width, height,
            sc.width, sc.height);
    }
    return ensure();
}

// a picture drawn on the CPU (premultiplied RGBA or BGRA, rows top-down) into an acquired image of a quad's swapchain;
// false: no image. The image is released after the frame's submission, as the eyes' are
bool upload_picture(VkCommandBuffer cmd, int swapchain, const std::vector<uint8_t>& pixels, uint32_t width, uint32_t height) {
    if (pixels.size() < size_t(width) * height * 4) return false;
    const UploadSlice up = allocate_upload(pixels.size(), 16);
    VkImage image = up.mapped ? xs().acquire(swapchain) : VK_NULL_HANDLE;
    if (!image) return false;
    std::memcpy(up.mapped, pixels.data(), pixels.size());
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    record::vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy region{};
    region.bufferOffset = up.offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    record::vkCmdCopyBufferToImage(cmd, up.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    record::vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &b);
    return true;
}

// The part of a screen's scan image that holds its picture, in that image's pixels (2026-11-04). scan_display_extent is
// the size the picture stands for: at a resolution factor below 1 (a Quest's or Steam Frame's fixed 0.75, dynamic
// resolution) the scan image is stored smaller (copy_to_scan: the source / its factor), so it was larger than the image:
// the eye copies, the upscalers and the GamePad panel's copy read past its right and bottom edges (a Quest 3 log: eye
// picture 1232x1333, scan image 924x1000), which the Adreno driver clamps: the right quarter of each eye's 3D view
// smeared into horizontal streaks (and the bottom). The desktop's presentation samples it normalized and is unaffected
static VkExtent3D scan_picture_extent(const Screen& screen) {
    VkExtent3D e = scan_display_extent(screen);
    if (!screen.scan) return e;
    const VkExtent3D& stored = screen.scan->extent;
    if (!e.width || !e.height) return stored;
    e.width = std::min(e.width, stored.width);
    e.height = std::min(e.height, stored.height);
    return e;
}

// VR dynamic resolution (docs/VR_DYNAMIC_RESOLUTION.md, vr/vr_dynres.h): the profile handed to dynres each headset
// frame (it takes effect at the next frame boundary; unchanged values cost a compare), its choice and why in F1 > VR
void update_dynamic_resolution() {
    const auto c = vr::config();
    vr::dynres_vr::Device d;
#if defined(__ANDROID__) || defined(__aarch64__)
    d.standalone = true;
    d.fixedMax = dynres::fixed_factor();  // (the profiles top out at the headset's fixed factor: vr_dynres.h)
#endif
    d.multiview = R.multiview;
    d.mesaDriver = R.driverID == VK_DRIVER_ID_MESA_TURNIP;
    vr::dynres_vr::Custom custom;
    custom.minScale = c.dynresMin;
    custom.maxScale = c.dynresMax;
    custom.gpuTargetPercent = c.dynresGpuTarget;
    custom.settleSeconds = c.dynresSettle;
    const auto choice = vr::dynres_vr::choose(c.dynamicResolution, custom, d);
    dynres::set_vr(true, choice.on, choice.profile.minScale, choice.profile.maxScale, choice.profile.gpuTarget,
                   choice.profile.settleSeconds);
    vr::dynres_vr::status_on().store(choice.on, std::memory_order_relaxed);
    vr::dynres_vr::status_why().store(choice.why, std::memory_order_relaxed);
}

// (diagnostics, WWHD_VR_SEQ_CAPTURE=1, 2026-11-12: "is right and left eyes in sync?") nine refreshes in a row while the
// stick turns (a real frame, its generated frames, the next real frames...), both eyes, as submitted: a band of rows
// of each headset image (no wait: copied in the frame's own commands) and the pose each was submitted with, its
// display time and field of view. Written to captures/genseq/ (seqN_fK_eE.png, seqN.csv) when the nine are in. Three
// sequences at most, 4 s apart, turning faster than 15 degrees a second
struct SeqEntry {
    Buffer buf{};
    bool have = false;
    int kind = 0;  // 0 real, 1 generated
    XrTime at = 0;
    vr::Quat pose;
    vr::Vec3 pos;
    float l = 0, r = 0, u = 0, d = 0, rate = 0;
    double planned = 0;  // the heading this refresh was meant to show (degrees: the game's heading plus the turn drawn or submitted)
    double gapMs = 0;    // (real) its display time less the moment it was drawn for; (generated) its time after that moment
};
struct SeqCapture {
    int seqs = 0, frame = -1;  // the sequence's next frame (-1: none running)
    static constexpr int kFrames = 9;
    SeqEntry e[kFrames][2];
    uint32_t width = 0, height = 0, y0 = 0, rows = 0;
    std::chrono::steady_clock::time_point last{};
} g_seq;
bool seq_capture_on() {
    static const bool on = [] {
        const char* e = std::getenv("WWHD_VR_SEQ_CAPTURE");
        return e && *e == '1';
    }();
    return on;
}
// the band copied for frame `f`, eye `eye`, from the acquired image `img` (in COLOR_ATTACHMENT_OPTIMAL)
void seq_copy(VkCommandBuffer cmd, int f, int eye, VkImage img) {
    SeqEntry& s = g_seq.e[f][eye];
    const auto& sc = xs().swapchain(eye);
    if (!g_seq.rows) {
        g_seq.width = sc.width, g_seq.height = sc.height;
        static const bool lower = [] {
            const char* e = std::getenv("WWHD_VR_SEQ_BAND");
            return e && !std::strcmp(e, "lower");
        }();
        // (WWHD_VR_SEQ_BAND=lower: the lower part of the view, where the body, hands, sword and shield are)
        const float from = lower ? 0.55f : 0.25f, part = lower ? 0.35f : 0.25f;
        g_seq.y0 = uint32_t(sc.height * from), g_seq.rows = std::min<uint32_t>(sc.height - g_seq.y0, uint32_t(sc.height * part));
    }
    if (sc.width != g_seq.width || sc.height != g_seq.height) return;
    s.buf = create_readback_buffer(VkDeviceSize(g_seq.width) * g_seq.rows * 4);
    if (!s.buf.buffer || !s.buf.mapped) return;
    vr_genframe::copy_rows(cmd, img, g_seq.width, g_seq.y0, g_seq.rows, s.buf.buffer);
    derive_dependency(s.buf.use, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    transition_buffer(s.buf, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    s.have = true;
}
void seq_note(int f, int eye, int kind, XrTime at, const vr::Pose& pose, const vr::Fov& fov, float rate, double planned = 0,
              double gapMs = 0) {
    SeqEntry& s = g_seq.e[f][eye];
    s.planned = planned, s.gapMs = gapMs;
    s.kind = kind, s.at = at, s.pose = pose.orientation, s.pos = pose.position, s.rate = rate;
    s.l = std::tan(fov.left), s.r = std::tan(fov.right), s.u = std::tan(fov.up), s.d = std::tan(fov.down);
}
// the nine are in: the GPU waited for once, the bands and the table written
void seq_finish() {
    flush();
    char dir[] = "captures/genseq";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    char path[256];
    std::snprintf(path, sizeof path, "%s/seq%d.csv", dir, g_seq.seqs);
    FILE* csv = std::fopen(path, "w");
    if (csv)
        std::fprintf(csv, "frame,eye,kind,display_ms,qx,qy,qz,qw,px,py,pz,tan_left,tan_right,tan_up,tan_down,turn_rate_deg_s,width,height,band_y0,band_rows,planned_heading_deg,gap_ms\n");
    const XrTime t0 = g_seq.e[0][0].at;
    const bool bgra = xs().swapchain(0).format == VK_FORMAT_B8G8R8A8_SRGB || xs().swapchain(0).format == VK_FORMAT_B8G8R8A8_UNORM;
    for (int f = 0; f < SeqCapture::kFrames; ++f)
        for (int eye = 0; eye < 2; ++eye) {
            SeqEntry& s = g_seq.e[f][eye];
            if (!s.have) continue;
            const size_t n = size_t(g_seq.width) * g_seq.rows;
            std::vector<uint8_t> rgba(n * 4);
            const auto* raw = static_cast<const uint8_t*>(s.buf.mapped);
            for (size_t i = 0; i < n; ++i) {
                rgba[i * 4 + 0] = bgra ? raw[i * 4 + 2] : raw[i * 4 + 0];
                rgba[i * 4 + 1] = raw[i * 4 + 1];
                rgba[i * 4 + 2] = bgra ? raw[i * 4 + 0] : raw[i * 4 + 2];
                rgba[i * 4 + 3] = 255;
            }
            defer_buffer(s.buf);
            s.buf = {};
            char png[256];
            std::snprintf(png, sizeof png, "%s/seq%d_f%d_e%d.png", dir, g_seq.seqs, f, eye);
            std::thread([path = std::string(png), w = g_seq.width, h = g_seq.rows, px = std::move(rgba)] {
                try {
                    write_rgba_png(path, w, h, px);
                } catch (...) {
                }
            }).detach();
            if (csv)
                std::fprintf(csv, "%d,%d,%s,%.3f,%.7f,%.7f,%.7f,%.7f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.3f,%u,%u,%u,%u,%.4f,%.3f\n", f, eye,
                             s.kind ? "gen" : "real", double(s.at - t0) * 1e-6, double(s.pose.x), double(s.pose.y),
                             double(s.pose.z), double(s.pose.w), double(s.pos.x), double(s.pos.y), double(s.pos.z), double(s.l),
                             double(s.r), double(s.u), double(s.d), double(s.rate) * 57.29578, g_seq.width, g_seq.height,
                             g_seq.y0, g_seq.rows, s.planned, s.gapMs);
            s.have = false;
        }
    if (csv) std::fclose(csv);
    LOG("[vr] refresh sequence capture %d written (%s)", g_seq.seqs, path);
    g_seq.frame = -1;
}
void record_eyes(VkCommandBuffer cmd) {
    if (!g_frame || !g_shouldRender || stereo::frame.mode != stereo::XR) return;
    update_dynamic_resolution();
    Screen* eyes[2] = {&R.tv, &stereoRight};
    for (Screen* s : eyes)
        if (!s->scan || !s->scan->image) return;
    VkImage targets[2] = {};
    const auto acquireStart = std::chrono::steady_clock::now();
    for (int eye = 0; eye < 2; ++eye) {
        targets[eye] = xs().acquire(eye);
        if (!targets[eye]) {
            if (eye) xs().release(0);
            return;
        }
    }
    if (g_times.acquire.size() < 4096)
        g_times.acquire.push_back(float(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - acquireStart).count()));
    // the upscaler (docs/VR_UPSCALING.md, vr/vr_upscale.h): Quest Super Resolution in the compositor, or the SGSR1
    // pass in place of the blit, or the blit
    const auto upscale = [&] {
        const auto c = vr::config();
        Surface* scan = eyes[0]->scan.get();
        const auto& sc = xs().swapchain(0);
        const VkExtent3D shown = scan_picture_extent(*eyes[0]);
        const float width = float(shown.width ? shown.width : scan->extent.width);
        vr::upscale::Caps caps;
        caps.superResolution = xs().super_resolution_offered();
        caps.shaderPass = vr_upscale::available(*scan, VkFormat(sc.format));
#if defined(__ANDROID__) || defined(__aarch64__)
        caps.standalone = true;
#endif
        caps.dlss = ngx_vk::active_mode() != ngx::Mode::Off;  // (decided at the start: ngx_vk.cpp probe)
        caps.temporal = motion_pass::on();                    // (SGSR2's motion vectors; started with the game)
        const auto choice = vr::upscale::choose(c.upscaler, caps, width > 0 ? float(sc.width) / width : 1.0f);
        static std::string last;
        char line[200];
        std::snprintf(line, sizeof line, "%s (eye picture %ux%u, headset image %ux%u, sharpness %.2f)", choice.why,
                      unsigned(width), unsigned(shown.height ? shown.height : scan->extent.height), sc.width, sc.height,
                      double(choice.cas ? c.casSharpness : c.upscaleSharpness));
        if (last != line) {
            last = line;
            LOG("[vr] upscaling: %s", line);
            vr::upscale::set_status(line);
        }
        xs().set_super_resolution(choice.superResolution);
        // the motion pass only while something uses it (motion_pass::set_needed): SGSR2 in use, the runtime's frame
        // synthesis (its swapchains: motion vectors each frame), or DLSS
        if (motion_pass::on())
            motion_pass::set_needed(choice.sgsr2 || (xs().has_synthesis_swapchains() && !vr_framesynth::held()) ||
                                    motion_pass::dlss_runs());
        struct Upscale { bool sgsr1, sgsr2, fsr1, cas, superResolution; float sharpness, fsrSharpness, casSharpness; bool casFxaa; };
        return Upscale{choice.sgsr1, choice.sgsr2, choice.fsr1, choice.cas, choice.superResolution, c.upscaleSharpness,
                       c.fsrSharpness, c.casSharpness, c.casFxaa};
    }();
    float eyepieceW = 0, eyepieceH = 0;
    const bool eyepiece = vr::eyepiece(eyepieceW, eyepieceH);
    // (docs/VR_UPSCALING.md, "The picture at its own size", 2026-11-08) with Quest Super Resolution the eye picture is
    // copied at its own size into the image's top-left corner and only that part submitted: the compositor's
    // SGSR-based filter then does the whole upscale to the display, in its one resample through the lenses, where the
    // blit stretched it bilinearly to the image's size first and left the filter only a softened picture to sharpen.
    // Not with the telescope's eyepiece (a part of the picture into a part of the image), the runtime's frame
    // synthesis (its motion and depth images are the image's size) or a picture larger than the image.
    // WWHD_VR_NATIVE_PICTURE=0 stretches it as before
    static const bool nativePicture = [] {
        const char* e = std::getenv("WWHD_VR_NATIVE_PICTURE");
        return !(e && e[0] == '0');
    }();
    for (int eye = 0; eye < 2; ++eye) {
        Surface* scan = eyes[eye]->scan.get();
        const auto& sc = xs().swapchain(eye);
        const VkExtent3D shown = scan_picture_extent(*eyes[eye]);
        g_eyeRect[eye][0] = g_eyeRect[eye][1] = 0;
        // SGSR2, then SGSR1 for an eye it could not draw (not with the telescope's eyepiece, which copies a part of the
        // picture into a part of the image)
        if (upscale.sgsr2 && !eyepiece &&
            vr_sgsr2::draw(cmd, eye, *scan, {shown.width, shown.height}, targets[eye], VkFormat(sc.format),
                           {sc.width, sc.height}))
            continue;
        if (upscale.fsr1 && !eyepiece &&
            vr_fsr1::draw(cmd, eye, *scan, {shown.width, shown.height}, targets[eye], VkFormat(sc.format),
                          {sc.width, sc.height}, upscale.fsrSharpness))
            continue;
        if (upscale.cas && !eyepiece &&
            vr_cas::draw(cmd, *scan, {shown.width, shown.height}, targets[eye], VkFormat(sc.format),
                         {sc.width, sc.height}, upscale.casSharpness, vr::fov_scale() < 1.0f ? 0.06f : 0.0f,
                         upscale.casFxaa))
            continue;
        if (upscale.sgsr1 && !eyepiece &&
            vr_upscale::draw(cmd, *scan, {shown.width, shown.height}, targets[eye], VkFormat(sc.format),
                             {sc.width, sc.height}, upscale.sharpness))
            continue;
        transition_image(scan, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_READ_BIT);
        // OpenXR (Vulkan): acquired images are in COLOR_ATTACHMENT_OPTIMAL and must be again at release
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = targets[eye];
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        record::vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &b);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(shown.width ? shown.width : scan->extent.width),
                              int32_t(shown.height ? shown.height : scan->extent.height), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[1] = {int32_t(sc.width), int32_t(sc.height), 1};
        const int32_t pw = blit.srcOffsets[1].x, ph = blit.srcOffsets[1].y;
        const bool native = nativePicture && upscale.superResolution && !eyepiece && pw > 0 && ph > 0 &&
                            uint32_t(pw) <= sc.width && uint32_t(ph) <= sc.height && !xs().has_synthesis_swapchains();
        if (native) {
            blit.dstOffsets[1] = {pw, ph, 1};
            g_eyeRect[eye][0] = pw, g_eyeRect[eye][1] = ph;
            static int logged[2][2] = {};
            if (logged[eye][0] != pw || logged[eye][1] != ph) {
                logged[eye][0] = pw, logged[eye][1] = ph;
                LOG("[vr] %s eye: its %dx%d picture submitted at its own size (of the %ux%u image), Quest Super "
                    "Resolution upscaling it (WWHD_VR_NATIVE_PICTURE=0 stretches it first)",
                    eye ? "right" : "left", pw, ph, sc.width, sc.height);
            }
        }
        // the telescope's eyepiece (docs/VR_TELESCOPE.md): black around it, only its centred part copied
        if (float ew, eh; vr::eyepiece(ew, eh)) {
            VkClearColorValue black{};
            black.float32[3] = 1.0f;
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            record::vkCmdClearColorImage(cmd, targets[eye], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
            // (the eyepiece is in the UI layer: in each eye's picture its area is where the quad shows it, eye_part;
            // centred when the eye pictures cover the drawn field)
            float x0 = 0, x1 = 1, y0 = 0, y1 = 1;
            if (!eye_part(eye, ew, eh, x0, x1, y0, y1)) {
                x0 = 0.5f - 0.5f * ew, x1 = 0.5f + 0.5f * ew;
                y0 = 0.5f - 0.5f * eh, y1 = 0.5f + 0.5f * eh;
            }
            auto part = [](int32_t full, float s0, float s1, int32_t& a, int32_t& b) {
                a = std::clamp<int32_t>(int32_t(std::lround(float(full) * s0)), 0, full - 1);
                b = std::clamp<int32_t>(int32_t(std::lround(float(full) * s1)), a + 1, full);
            };
            part(blit.srcOffsets[1].x, x0, x1, blit.srcOffsets[0].x, blit.srcOffsets[1].x);
            part(blit.srcOffsets[1].y, y0, y1, blit.srcOffsets[0].y, blit.srcOffsets[1].y);
            part(int32_t(sc.width), x0, x1, blit.dstOffsets[0].x, blit.dstOffsets[1].x);
            part(int32_t(sc.height), y0, y1, blit.dstOffsets[0].y, blit.dstOffsets[1].y);
        }
        // the eye's picture was drawn for the eye's field of view: it fills the image (pixels need not be square)
        record::vkCmdBlitImage(cmd, scan->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, targets[eye],
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                               native ? VK_FILTER_NEAREST : scan->linearFiltering ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        if (native) {
            // the picture's last column and row once more just beyond it: the compositor's filter reading half a
            // pixel past the submitted part finds the edge's own colour, not an older frame's pixels there
            VkImageBlit edge[3]{};
            for (VkImageBlit& e : edge) e.srcSubresource = e.dstSubresource = blit.srcSubresource;
            uint32_t n = 0;
            if (uint32_t(pw) < sc.width) {
                edge[n].srcOffsets[0] = {pw - 1, 0, 0}, edge[n].srcOffsets[1] = {pw, ph, 1};
                edge[n].dstOffsets[0] = {pw, 0, 0}, edge[n].dstOffsets[1] = {pw + 1, ph, 1};
                ++n;
            }
            if (uint32_t(ph) < sc.height) {
                edge[n].srcOffsets[0] = {0, ph - 1, 0}, edge[n].srcOffsets[1] = {pw, ph, 1};
                edge[n].dstOffsets[0] = {0, ph, 0}, edge[n].dstOffsets[1] = {pw, ph + 1, 1};
                ++n;
            }
            if (uint32_t(pw) < sc.width && uint32_t(ph) < sc.height) {
                edge[n].srcOffsets[0] = {pw - 1, ph - 1, 0}, edge[n].srcOffsets[1] = {pw, ph, 1};
                edge[n].dstOffsets[0] = {pw, ph, 0}, edge[n].dstOffsets[1] = {pw + 1, ph + 1, 1};
                ++n;
            }
            if (n)
                record::vkCmdBlitImage(cmd, scan->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, targets[eye],
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, n, edge, VK_FILTER_NEAREST);
        }
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        record::vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &b);
    }
    g_recorded = true;
    // (docs/VR_HANDS_LAYER_PLAN.md step 3) each eye's hands image into its hands-layer image (the eyes' size: their parts
    // and crops line up), sent over the eyes' layer with the frame
    g_handsRecorded = false;
    g_handsShow = false;
    {  // (diagnostics) this frame's hands images, and whether they go out (HandsMiss)
        Surface *d0 = hands_layer_drawn(0), *d1 = hands_layer_drawn(1);
        if (d0 || d1) {
            ++g_handsMiss.frames;
            if (!d0 || !d1) ++g_handsMiss.oneEye;
        }
        static auto last = std::chrono::steady_clock::now();
        if (const auto now = std::chrono::steady_clock::now(); now - last > std::chrono::seconds(10)) {
            last = now;
            if (g_handsMiss.frames)
                LOG("[vr hands] (10 s) frames with his hands in their layer %u: left out with one eye's only %u, an eye's "
                    "pass failed %u, no layer image %u; drawn without his depth %u",
                    g_handsMiss.frames, g_handsMiss.oneEye, g_handsMiss.passFailed, g_handsMiss.noImage, g_handsMiss.noDepth);
            g_handsMiss = {};
        }
    }
    // (diagnostics, 2026-11-14: releasing the hands images took ~3 ms on SteamVR) WWHD_VR_HANDS_SWAPCHAIN=skip: never
    // acquired nor released (nothing sent); =norecord: acquired and released, nothing written into them (run it with
    // WWHD_VR_HANDS_SUBMIT=none)
    static const int swapchainTest = [] {
        const char* e = std::getenv("WWHD_VR_HANDS_SWAPCHAIN");
        const int m = e && !std::strcmp(e, "skip") ? 1 : e && !std::strcmp(e, "norecord") ? 2 : 0;
        if (m) LOG("[vr hands] WWHD_VR_HANDS_SWAPCHAIN=%s", e);
        return m;
    }();
    if (Surface *h0 = hands_layer_drawn(0), *h1 = hands_layer_drawn(1);
        swapchainTest != 1 && h0 && h1 && xs().ensure_hands(xs().swapchain(0).width, xs().swapchain(0).height)) {
        const bool shared = vr::xr::hands_shared();
        xs().set_hands_premultiplied(vr_genframe::hands_coverage_from_depth());
        VkImage hi[2] = {xs().acquire(vr::xr::Session::kHands), VK_NULL_HANDLE};
        if (hi[0]) hi[1] = shared ? hi[0] : xs().acquire(vr::xr::Session::kHands + 1);
        if (hi[0] && hi[1]) {
            Surface* hs[2] = {h0, h1};
            bool written = true;
            end_encoder();
            for (int eye = 0; eye < 2 && swapchainTest != 2; ++eye) {
                const auto& hsc = xs().swapchain(vr::xr::Session::kHands + (shared ? 0 : eye));
                const uint32_t partW = shared ? hsc.width / 2 : hsc.width, partX = shared ? uint32_t(eye) * partW : 0;
                // (the edges filled with the hands' own colour: a compositor filtering the layer's edge then blends that,
                // not the empty pixels' black, which showed as a black outline round his hands in a headset run)
                // (and his pixels behind what the world drew after him left out: the game's depth test, as without the
                // layer; "Link's sword clips through objects", 2026-11-13)
                Surface *linkDepth = nullptr, *worldDepth = nullptr;
                if (!hands_layer_depths(eye, linkDepth, worldDepth)) ++g_handsMiss.noDepth;
                // (any size: the pass samples his image across its part; a standalone headset's pictures are smaller than
                // its images, upscaled into them: with the sizes required equal the shared image was left unwritten there)
                if (vr_genframe::dilate(cmd, *hs[eye], hi[eye], VkFormat(hsc.format), partW, hsc.height, linkDepth, worldDepth,
                                        partX))
                    continue;
                if (shared) {  // (the blit below writes a whole image: not for the shared one; its half left unwritten)
                    written = false;
                    continue;
                }
                transition_image(hs[eye], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_ACCESS_TRANSFER_READ_BIT);
                VkImageMemoryBarrier hb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                hb.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                hb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                hb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                hb.image = hi[eye];
                hb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                record::vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                             0, 0, nullptr, 0, nullptr, 1, &hb);
                VkImageBlit blit{};
                blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit.srcOffsets[1] = {int32_t(hs[eye]->extent.width), int32_t(hs[eye]->extent.height), 1};
                blit.dstOffsets[1] = {int32_t(hsc.width), int32_t(hsc.height), 1};
                record::vkCmdBlitImage(cmd, hs[eye]->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hi[eye],
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
                hb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                hb.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                hb.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                record::vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                             0, 0, nullptr, 0, nullptr, 1, &hb);
            }
            g_handsRecorded = true;
            g_handsShow = written || swapchainTest == 2;
            if (!written) ++g_handsMiss.passFailed;
        } else {
            ++g_handsMiss.noImage;
            if (hi[0]) xs().release(vr::xr::Session::kHands);
        }
    }
    // (docs/VR_HANDS_LAYER_PLAN.md, diagnostics: WWHD_VR_HANDS_ALPHA=1) once, 20 s into first-person play, the left eye's
    // hands image with its alpha (its coverage: how the headset blends its edge over the world) and the eye's picture,
    // as captures/hands/alpha_hands.png and alpha_picture.png (one hitch)
    {
        static const bool alphaDump = [] { const char* e = std::getenv("WWHD_VR_HANDS_ALPHA"); return e && *e == '1'; }();
        static bool done = false;
        static std::chrono::steady_clock::time_point firstSeen{};
        Surface* h = alphaDump && !done ? hands_layer_drawn(0) : nullptr;
        if (h && h->image) {
            const auto now = std::chrono::steady_clock::now();
            if (firstSeen.time_since_epoch().count() == 0) firstSeen = now;
            if (now - firstSeen > std::chrono::seconds(20)) {
                done = true;
                const uint32_t w = h->extent.width, hh = h->extent.height;
                Buffer buf = create_readback_buffer(VkDeviceSize(w) * hh * 4);
                if (buf.mapped) {
                    transition_image(h, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_ACCESS_TRANSFER_READ_BIT);
                    VkBufferImageCopy region{};
                    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, h->baseLayer, 1};
                    region.imageExtent = {w, hh, 1};
                    record::vkCmdCopyImageToBuffer(command_buffer(), h->image, h->layout, buf.buffer, 1, &region);
                    derive_dependency(buf.use, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
                    transition_buffer(buf, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
                    flush();
                    std::vector<uint8_t> px(static_cast<const uint8_t*>(buf.mapped),
                                            static_cast<const uint8_t*>(buf.mapped) + size_t(w) * hh * 4);
                    std::error_code ec;
                    std::filesystem::create_directories("captures/hands", ec);
                    try {
                        write_rgba_png("captures/hands/alpha_hands.png", w, hh, px);
                        if (eyes[0]->scan) {
                            auto pic = read_surface_rgba(*eyes[0]->scan, true);
                            write_rgba_png("captures/hands/alpha_picture.png", eyes[0]->scan->extent.width,
                                           eyes[0]->scan->extent.height, pic);
                        }
                    } catch (...) {
                    }
                    LOG("[vr hands] the hands image with its alpha written (captures/hands/alpha_hands.png)");
                }
                defer_buffer(buf);
            }
        }
    }
    // (diagnostics, 2026-11-14: "transition still show 2 black cut outs instead of 1") the frames a picture transition
    // drew: each eye's picture and the UI layer's image as PNGs in captures/transition/, 0.25 s apart, 6 in a session
    // (each waits for the GPU: a hitch, during a transition)
    // (2026-11-14: the first version saved its 6 at the transition's first frame, all black: each save's hitch was longer
    // than the 0.25 s between them) A sequence from a transition's first frame: both eyes' pictures and the UI layer's
    // image (half size) at frames +0 .. +180 of it (counted in frames, so the saves' hitches do not bunch them), 3
    // sequences a session. The trace's lines in between are numbered by the picture they lead to (draw.cpp)
    {
        static constexpr int kAt[] = {0, 5, 10, 20, 30, 45, 60, 90, 120, 180};
        static constexpr int kShots = int(sizeof kAt / sizeof kAt[0]);
        static int seqs = 0, seqFrame = -1, shot = 0;
        // (2026-11-14, in the headset: the saves' hitches left the game too slow to play) only on a test start,
        // WWHD_VR_TRANSITION_CAPTURE=1; never in play (a video of the headset is the way to see a transition)
        static const bool on = [] { const char* e = std::getenv("WWHD_VR_TRANSITION_CAPTURE"); return e && *e == '1'; }();
        const bool drawn = stereo::transitionDrawn.exchange(false) && on;
        if (drawn && seqFrame < 0 && seqs < 3) {
            ++seqs, seqFrame = 0, shot = 0;
            stereo::transitionShot.store(seqs * 100 + 1);
            stereo::transitionTraceGen.fetch_add(1);
            LOG("[vr] transition capture %d begins (captures/transition/s%d_*.png)", seqs, seqs);
        }
        if (seqFrame >= 0) {
            if (shot < kShots && seqFrame == kAt[shot]) {
                std::error_code ec;
                std::filesystem::create_directories("captures/transition", ec);
                auto save = [&](Surface* sf, const char* what) {
                    if (!sf || !sf->image) return;
                    try {
                        const auto px = read_surface_rgba(*sf, true);
                        const uint32_t w = sf->extent.width, h = sf->extent.height, ow = w / 2, oh = h / 2;
                        if (px.size() < size_t(w) * h * 4 || !ow || !oh) return;
                        std::vector<uint8_t> out(size_t(ow) * oh * 4);
                        for (uint32_t y = 0; y < oh; ++y)
                            for (uint32_t x = 0; x < ow; ++x)
                                for (int ch = 0; ch < 4; ++ch) {
                                    const size_t a = ((size_t(y) * 2) * w + x * 2) * 4 + ch, b = a + size_t(w) * 4;
                                    out[(size_t(y) * ow + x) * 4 + ch] = uint8_t((px[a] + px[a + 4] + px[b] + px[b + 4]) / 4);
                                }
                        char path[160];
                        std::snprintf(path, sizeof path, "captures/transition/s%d_f%03d_%s.png", seqs, kAt[shot], what);
                        write_rgba_png(path, ow, oh, out);
                    } catch (...) {
                    }
                };
                save(eyes[0]->scan.get(), "eye0");
                save(eyes[1]->scan.get(), "eye1");
                save(ui_layer_drawn_last(), "ui");
                LOG("[vr] transition capture %d: picture %d at frame +%d written (s%d_f%03d_eye0/eye1/ui.png)", seqs,
                    shot + 1, kAt[shot], seqs, kAt[shot]);
                ++shot;
                stereo::transitionShot.store(seqs * 100 + shot + 1);  // (the next picture's draws, draw.cpp's trace)
                stereo::transitionTraceGen.fetch_add(1);
            }
            stereo::transitionTrace.store(shot < kShots ? 2 : 0);  // (traced until the last picture)
            if (shot >= kShots) seqFrame = -1;
            else ++seqFrame;
        }
    }
    // (docs/VR_HANDS_LAYER_PLAN.md step 2, diagnostics: WWHD_VR_HANDS_DUMP=1) each eye's picture and hands image as PNGs
    // in captures/hands/, every 3 s, 12 times (each waits for the GPU: a hitch)
    {
        static const bool dump = [] { const char* e = std::getenv("WWHD_VR_HANDS_DUMP"); return e && *e == '1'; }();
        static int dumps = 0;
        static auto last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        // (every 6 s while Link's hands image is drawn (gameplay, first person), 30 times: the whole session, not its
        // first minute of loading)
        if (dump && dumps < 30 && now - last > std::chrono::seconds(6) && hands_layer_drawn(0)) {
            last = now;
            ++dumps;
            std::error_code ec;
            std::filesystem::create_directories("captures/hands", ec);
            for (int eye = 0; eye < 1; ++eye) {  // (the left eye: enough to see what is drawn)
                auto save = [&](Surface* sf, const char* what) {
                    if (!sf || !sf->image) return;
                    try {
                        auto px = read_surface_rgba(*sf, true);
                        char path[160];
                        std::snprintf(path, sizeof path, "captures/hands/dump%d_e%d_%s.png", dumps, eye, what);
                        write_rgba_png(path, sf->extent.width, sf->extent.height, px);
                    } catch (...) {
                    }
                };
                save(eyes[eye]->scan.get(), "picture");
                save(hands_layer_drawn(eye), "hands");
            }
            LOG("[vr hands] dump %d written (captures/hands/)", dumps);
        }
    }
    // (diagnostics, the steady hands test, WWHD_VR_HANDS_AB) the left eye's picture (a quarter of its size) every 3 s in
    // first-person play, 120 times, named by the test's phase; the hands image too when drawn (each waits for the GPU)
    if (vr::hands_ab_mode() >= 0) {
        static int abDumps = 0;
        static auto abLast = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (abDumps < 120 && now - abLast > std::chrono::seconds(3) && vr::game::hands_layer_view(true)) {
            abLast = now;
            ++abDumps;
            const int mode = vr::hands_ab_mode();
            std::error_code ec;
            std::filesystem::create_directories("captures/handstest", ec);
            auto save = [&](Surface* sf, const char* what) {
                if (!sf || !sf->image) return;
                try {
                    const auto px = read_surface_rgba(*sf, true);
                    const uint32_t w = sf->extent.width, h = sf->extent.height, k = 4, ow = w / k, oh = h / k;
                    if (px.size() < size_t(w) * h * 4 || !ow || !oh) return;
                    std::vector<uint8_t> out(size_t(ow) * oh * 4);
                    for (uint32_t y = 0; y < oh; ++y)
                        for (uint32_t x = 0; x < ow; ++x)
                            for (int ch = 0; ch < 4; ++ch) {
                                uint32_t sum = 0;
                                for (uint32_t dy = 0; dy < k; ++dy)
                                    for (uint32_t dx = 0; dx < k; ++dx)
                                        sum += px[((size_t(y) * k + dy) * w + x * k + dx) * 4 + ch];
                                out[(size_t(y) * ow + x) * 4 + ch] = uint8_t(sum / (k * k));
                            }
                    char path[160];
                    std::snprintf(path, sizeof path, "captures/handstest/d%03d_phase%d_%s.png", abDumps, mode, what);
                    write_rgba_png(path, ow, oh, out);
                } catch (...) {
                }
            };
            save(eyes[0]->scan.get(), "picture");
            save(hands_layer_drawn(0), "hands");
            LOG("[vr hands test] picture %d (phase %d) written to captures/handstest/", abDumps, mode);
        }
    }
    // (the refresh sequence capture) a real frame: a sequence begun here while the stick turns, or its next frame
    if (seq_capture_on()) {
        const auto now = std::chrono::steady_clock::now();
        if (g_seq.frame < 0 && g_seq.seqs < 10 && std::fabs(vr::view_turn_rate()) > 15.0f / 57.29578f &&
            vr::config().turnSmoothing && now - g_seq.last > std::chrono::seconds(3)) {
            ++g_seq.seqs, g_seq.frame = 0, g_seq.last = now;
            LOG("[vr] refresh sequence capture %d begins: %s view%s, the stick turn rate %.1f degrees a second", g_seq.seqs,
                vr::game::third_person_view() ? "third person" : "first person", vr::game::boat_view() ? " (the boat's)" : "",
                double(vr::view_turn_rate()) * 57.29578);
        }
        if (g_seq.frame >= 0 && g_seq.frame < SeqCapture::kFrames)
            for (int eye = 0; eye < 2; ++eye) seq_copy(cmd, g_seq.frame, eye, targets[eye]);
    }
    // each eye's motion vectors and depth for the runtime (docs/VR_FRAME_SYNTHESIS.md), when it takes them
    if (vr_framesynth::active()) vr_framesynth::record(cmd);
    // (docs/VR_EYE_PROJECTION.md) the game's UI layer's picture into its quad's image (each eye with its own projection)
    g_uiRecorded = false;
    if (Surface* ui = ui_layer_image(); ui && ensure_swapchain(vr::xr::Session::kUi, ui->extent.width, ui->extent.height,
                                             [&] { return xs().ensure_ui(ui->extent.width, ui->extent.height); })) {
        if (VkImage img = xs().acquire(vr::xr::Session::kUi)) {
            const auto& us = xs().swapchain(vr::xr::Session::kUi);
            transition_image(ui, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_TRANSFER_READ_BIT);
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = img;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            record::vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         0, 0, nullptr, 0, nullptr, 1, &b);
            VkImageBlit blit{};
            blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = {int32_t(ui->extent.width), int32_t(ui->extent.height), 1};
            blit.dstOffsets[1] = {int32_t(us.width), int32_t(us.height), 1};
            record::vkCmdBlitImage(cmd, ui->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            record::vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                         0, 0, nullptr, 0, nullptr, 1, &b);
            g_uiRecorded = true;
        }
    }
    // (diagnostics, 2026-11-09: dialogue letters left dark) frames that brought no new UI picture right after frames that
    // did: the quad keeps the picture it had, so a dropped frame shows the text a step behind. Counted, logged every 10 s
    // when there were any
    {
        static bool hadUi = false;
        static uint32_t dropped = 0, frames = 0;
        if (!g_uiRecorded && hadUi) ++dropped;
        hadUi = g_uiRecorded;
        if (++frames >= 900) {
            if (dropped) LOG("[vr] UI layer: %u frames with no new UI picture right after one that had it (of %u)", dropped, frames);
            dropped = frames = 0;
        }
    }
    // the GamePad picture into the panel's image (or, while the VR menu is open, the settings: docs/VR_MENU.md)
    g_panelRecorded = false;
    Surface* drc = R.drc.scan ? R.drc.scan.get() : nullptr;
    // (2026-11-12, a PC log: a frame of 23 to 31 ms each time the VR menu opened or closed) one panel image for the
    // GamePad screen and the VR menu: made at the larger of their sizes and never smaller than it was, each picture
    // drawn into its top left part (g_panel.pixelWidth/Height, the quad's image rect). Made again at another size each
    // switch, the swapchain's old images had to wait for the whole GPU first (ensure_swapchain). Only a larger picture
    // than any before makes it again (at most once or twice a session)
    const auto panelImage = [](uint32_t w, uint32_t h) -> VkImage {
        const auto& sc = xs().swapchain(vr::xr::Session::kPanel);
        const uint32_t pw = std::max({sc.width, w, kMenuPanelWidth}), ph = std::max({sc.height, h, kMenuPanelHeight});
        if (!ensure_swapchain(vr::xr::Session::kPanel, pw, ph, [&] { return xs().ensure_panel(pw, ph); })) return VK_NULL_HANDLE;
        g_panel.pixelWidth = w, g_panel.pixelHeight = h;
        return xs().acquire(vr::xr::Session::kPanel);
    };
    if (g_panelShown && g_panelMenu) {
        VkImage panel = panelImage(kMenuPanelWidth, kMenuPanelHeight);
        if (panel) {
            const auto& sc = xs().swapchain(vr::xr::Session::kPanel);
            VkImageView& view = g_panelViews[panel];
            if (!view) {
                VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                vi.image = panel;
                vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
                vi.format = VkFormat(sc.format);
                vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                if (vkCreateImageView(R.device, &vi, nullptr, &view) != VK_SUCCESS) view = VK_NULL_HANDLE;
            }
            VkImageLayout layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;  // (acquired images are, and are again at release)
            if (view && compose_overlay_into(panel, view, layout, {kMenuPanelWidth, kMenuPanelHeight}, VkFormat(sc.format))) g_panelRecorded = true;
            else {
                // nothing drawn (the overlay's first frame): the image still goes back cleared, in its layout
                VkClearColorValue black{};
                black.float32[3] = 1.0f;
                const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = panel;
                b.subresourceRange = range;
                record::vkCmdPipelineBarrier(command_buffer(), VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                             0, 0, nullptr, 0, nullptr, 1, &b);
                record::vkCmdClearColorImage(command_buffer(), panel, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
                b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                record::vkCmdPipelineBarrier(command_buffer(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                             0, 0, nullptr, 0, nullptr, 1, &b);
                g_panelRecorded = true;
            }
        }
    } else if (g_panelShown && drc && drc->image) {
        const VkExtent3D shown = scan_picture_extent(R.drc);
        const uint32_t w = shown.width ? shown.width : drc->extent.width, h = shown.height ? shown.height : drc->extent.height;
        VkImage panel = panelImage(w, h);
        if (panel) {
            transition_image(drc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = panel;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            record::vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         0, 0, nullptr, 0, nullptr, 1, &b);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = {int32_t(w), int32_t(h), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[1] = {int32_t(w), int32_t(h), 1};  // (its part of the panel's image: g_panel.pixelWidth)
            record::vkCmdBlitImage(cmd, drc->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, panel,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            record::vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                         0, 0, nullptr, 0, nullptr, 1, &b);
            g_panelRecorded = true;
        }
    }
    // the item wheel's picture (drawn on the CPU when its contents change) into its quad's image
    const int64_t format = xs().format();
    const bool bgra = format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM;
    g_wheelRecorded = false;
    vr::WheelView wheel;
    if (vr::wheel_view(wheel) && ensure_swapchain(vr::xr::Session::kWheel, vr::kWheelPixels, vr::kWheelPixels,
                                                [] { return xs().ensure_wheel(vr::kWheelPixels, vr::kWheelPixels); }) &&
        upload_picture(cmd, vr::xr::Session::kWheel, vr::wheel_pixels(wheel, bgra), vr::kWheelPixels, vr::kWheelPixels)) {
        g_wheel.pose = wheel.pose;
        g_wheel.width = g_wheel.height = wheel.size;
        g_wheelRecorded = true;
    }
    // the wrist HUD's picture (vr_wrist_hud.h): one image, a part of it per panel's quad
    g_wristRecorded = false;
    vr::WristHudView hud;
    if (vr::wrist_hud_view(hud) && ensure_swapchain(vr::xr::Session::kWrist, vr::kWristHudWidth, vr::kWristHudHeight,
                                                    [] { return xs().ensure_wrist(vr::kWristHudWidth, vr::kWristHudHeight); }) &&
        upload_picture(cmd, vr::xr::Session::kWrist, vr::wrist_hud_pixels(hud, bgra), vr::kWristHudWidth,
                       vr::kWristHudHeight)) {
        for (int i = 0; i < vr::kWristPanels; ++i) {
            const vr::WristRect& r = vr::kWristRects[i];
            g_wrist[i].quad.visible = hud.visible[i];
            g_wrist[i].quad.pose = hud.pose[i];
            g_wrist[i].quad.width = hud.width[i];
            g_wrist[i].quad.height = hud.height[i];
            g_wrist[i].x = r.x, g_wrist[i].y = r.y, g_wrist[i].w = r.w, g_wrist[i].h = r.h;
            g_wrist[i].inView = hud.inView[i];  // (the Free Roam label: held in the view)
        }
        g_wristRecorded = true;
    }
    // the aiming laser (vr_laser.h): a beam from the item to the sight's point, and a dot there, parts of one picture
    g_laserRecorded = false;
    vr::LaserView laser;
    if (vr::laser_view(laser) && ensure_swapchain(vr::xr::Session::kLaser, vr::kLaserWidth, vr::kLaserHeight,
                                                 [] { return xs().ensure_laser(vr::kLaserWidth, vr::kLaserHeight); })) {
        vr::Pose beam, dot;
        float len = 0, size = 0;
        const bool haveBeam = vr::laser_beam_pose(laser.from, laser.to, laser.eye, beam, len);
        const bool haveDot = vr::laser_dot_pose(laser.to, laser.eye, dot, size);
        if ((haveBeam || haveDot) &&
            upload_picture(cmd, vr::xr::Session::kLaser, vr::laser_pixels(bgra), vr::kLaserWidth, vr::kLaserHeight)) {
            auto set = [](vr::xr::SubQuad& q, bool visible, const vr::Pose& pose, float w, float h, const vr::LaserRect& r) {
                q.quad.visible = visible;
                q.quad.pose = pose;
                q.quad.width = w;
                q.quad.height = h;
                q.x = r.x, q.y = r.y, q.w = r.w, q.h = r.h;
            };
            set(g_laser[0], haveBeam, beam, vr::kLaserWidthMetres, len, vr::kLaserBeamRect);
            set(g_laser[1], haveDot, dot, size, size, laser.valid ? vr::kLaserValidRect : vr::kLaserInvalidRect);
            g_laserRecorded = true;
        }
    }
}

bool skip_desktop_gamepad() {
    return stereo::frame.mode == stereo::XR && vr::active() && !vr::config().desktopGamePad;
}

bool skip_window_tv() {
#if defined(__ANDROID__)
    // (a PC keeps its desktop window: a mirror for the room, captures and streams)
    return stereo::frame.mode == stereo::XR && vr::active();
#elif defined(WWHD_STEAM_FRAME_NATIVE) || (defined(__linux__) && defined(__aarch64__))
    // (Steam Frame - Native, 2026-11-14, the user's choice: the window is not seen in the headset either, and composing
    // its picture cost the Quest's GPU more pixels than both eyes; swap() records the eyes, as on a PC whose window had
    // no image ready). WWHD_VR_DESKTOP_WINDOW=1 keeps presenting it
    static const bool keep = [] { const char* e = std::getenv("WWHD_VR_DESKTOP_WINDOW"); return e && *e == '1'; }();
    return !keep && stereo::frame.mode == stereo::XR && vr::active();
#else
    return false;
#endif
}

bool eyes_pending() { return g_frame && g_shouldRender && !g_recorded; }
void frame_end() {
    if (!g_frame) return;
    g_frame = false;
    vr::xr::Views v;
    // the views the frame was drawn with; a frame the game drew without its camera (title screen, menus,
    // loading) is shown at the head's predicted pose: it stays in front of the eyes
    double gameTime = -1;
    bool views = g_recorded && vr::views(g_frameViews, v, &gameTime);
    g_stats.withViews += views;
    if (g_recorded && !views && xs().locate_views(xs().predicted_display_time(), v)) {
        float eyeX[2];
        v = vr::as_rendered(v, eyeX);
        views = true;
    }
    // (docs/VR_CAMERA_AUDIT.md, "Both eyes, one snapshot") the eyes go to the compositor where they were drawn: the
    // head of the frame's views, each eye at the offset this frame's eye passes used (stereo::frame, latched at the
    // swap before from frame_begin's located eyes). The views' own eye offsets come from the game thread's locate;
    // a runtime whose eye positions move between two locates (an eye-tracked IPD) would otherwise be reprojected
    // from eyes the picture was not drawn from
    if (views && stereo::frame.mode == stereo::XR && stereo::frame.eyeOffset[0] < 0 && stereo::frame.eyeOffset[1] > 0) {
        const float scale = vr::world_scale();
        if (scale > 0)
            for (int e = 0; e < 2; ++e) v.eye[e].pose = vr::eye_pose(v.head, stereo::frame.eyeOffset[e] / scale);
    }
    // (a Quest, no views at all, a long tracking loss: the frame keeps the last submitted views, which the runtime reprojects to
    // the head, rather than going out without its picture layer, a blank flash in the headset)
    if (g_recorded && !views && xs().quest() && g_shown.valid && g_shown.drawn) {
        v = g_shown.views;
        views = true;
        ++g_stats.viewsReused;
    }
    vr::xr::Quad panel = g_panel;
    panel.visible = g_panelRecorded;
    vr::xr::Quad wheel = g_wheel;
    wheel.visible = g_wheelRecorded;
    // the images released and xrEndFrame: on the end-frame thread (its job is a copy), or here
    // (the eyes' pictures at their own size: the part of each image written, record_eyes)
    if (g_recorded)
        for (int e = 0; e < 2; ++e) v.eye[e].rectWidth = g_eyeRect[e][0], v.eye[e].rectHeight = g_eyeRect[e][1];
    // (2026-11-12) the real frame turned to the refresh it lands on: drawn for its views' time, it is shown at this frame's
    // predicted display time, a refresh before or after in a third of the frames of a headset run at Steady 30 (the
    // view stepped back or stood still for a refresh during a stick turn: the doubling). The stick's turn over the gap is
    // submitted in its pose (an image drawn at heading h0 shown at h0 + w dt: submitted turned by -w dt about the head,
    // as frames shown again, show_again); the runtime turns it as it does for the head. Capped 45 and Steady 30 with
    // Generated frames or Turn smoothing on (vr.frameRate half or original); WWHD_VR_TIME_CORRECT=0 off
    float correctedTurn = 0;
    vr::xr::Views submitted = v;
    {
        static const bool timeCorrect = [] {
            const char* e = std::getenv("WWHD_VR_TIME_CORRECT");
            return !(e && *e == '0');
        }();
        const vr::FrameRate mode = vr::config().frameRate;
        const float rate = vr::view_turn_rate_for(v.time);
        const bool lowRate = mode == vr::FrameRate::Half || mode == vr::FrameRate::Original ||
                             (mode == vr::FrameRate::Auto && low_rate_repeats(xs().refresh_rate(), 0) > 0);
        if (timeCorrect && views && v.valid && v.time && lowRate &&
            vr::config().turnSmoothing && std::fabs(rate) > 0.05f) {
            const double gap = double(xs().predicted_display_time() - v.time) * 1e-9;
            if (std::fabs(gap) < 0.05) {
                correctedTurn = float(double(rate) * gap);
                const vr::Quat r = vr::yaw_quat(-correctedTurn);
                const vr::Vec3 pivot = submitted.head.position;
                submitted.head.orientation = vr::normalize(r * submitted.head.orientation);
                for (auto& e : submitted.eye) {
                    e.pose.position = pivot + vr::rotate(r, e.pose.position - pivot);
                    e.pose.orientation = vr::normalize(r * e.pose.orientation);
                }
            }
        }
    }
    if (seq_capture_on() && g_seq.frame >= 0 && g_seq.frame < SeqCapture::kFrames && g_seq.e[g_seq.frame][0].have) {
        for (int eye = 0; eye < 2; ++eye)
            seq_note(g_seq.frame, eye, 0, xs().predicted_display_time(), submitted.eye[eye].pose, submitted.eye[eye].fov,
                     vr::view_turn_rate_for(v.time), (double(v.heading) + double(correctedTurn)) * 57.29578,
                     double(xs().predicted_display_time() - v.time) * 1e-6);
        if (++g_seq.frame == SeqCapture::kFrames) seq_finish();
    }
    end_thread::Job job;
    job.views = submitted;
    job.drawn = views;
    job.eyes = g_recorded;
    job.panel = g_panelRecorded, job.wheel = g_wheelRecorded, job.wrist = g_wristRecorded, job.laser = g_laserRecorded;
    job.hands = g_handsRecorded && g_recorded && views;
    job.handsShow = g_handsShow;
    g_handsRecorded = g_handsShow = false;
    job.panelQuad = panel, job.wheelQuad = wheel;
    // the UI layer's quad: head-locked, kUiDistance ahead, the size the symmetric field covers there
    // (behind the GamePad panel while it is shown: nearer, the game's UI looked in front of the panel drawn over it)
    if (vr::Fov f; g_uiRecorded && vr::projection(f)) {
        const float d = job.panel ? std::max(kUiDistance, panel.distance + 1.0f) : kUiDistance;
        job.ui = true;
        job.uiQuad.visible = true;
        job.uiQuad.pose.position = {0.0f, 0.0f, -d};
        job.uiQuad.width = 2.0f * d * std::tan(f.right);
        job.uiQuad.height = 2.0f * d * std::tan(f.up);
        // (docs/VR_CUTSCENE_TEXT.md, "Dialogue in the UI layer") a conversation's text held in the world: the quad in
        // the tracking space, d along the held direction from the head, facing it (upright), the compositor keeping it
        // there at the display's head pose
        if (vr::Vec3 dir; vr::talk_text_dir(dir) && views && v.valid) {
            job.uiQuad.tracking = true;
            job.uiQuad.pose.position = v.head.position + dir * d;
            job.uiQuad.pose.orientation = vr::look_rotation(dir, vr::Vec3{0, 1, 0});
        }
    }
    g_uiRecorded = false;
    for (int i = 0; i < vr::kWristPanels; ++i) job.wristQuads[i] = g_wrist[i];
    for (int i = 0; i < 2; ++i) job.laserQuads[i] = g_laser[i];
    end_thread::submit(job);
    g_stats.submitted += views;
    // (kept for headset_frame: its layers name the swapchains, whose images released here are the ones shown again)
    g_shown.valid = true;
    g_shown.views = v;
    g_shown.drawn = views;
    g_shown.turnRate = views ? vr::view_turn_rate_for(v.time) : 0.0f;  // (set for this frame, else none)
    g_shown.shownAt = xs().predicted_display_time();
    if (views && v.hasHeading) {
        note_shown(v.heading + correctedTurn, g_shown.shownAt, false, g_shown.turnRate);  // (as shown: corrected)
        const double lead = double(v.time - g_shown.shownAt) * 1e-6;  // ms
        if (std::fabs(lead) < 200.0) {
            ++g_even.leads;
            g_even.leadSum += lead, g_even.leadSq += lead * lead;
            g_even.leadLo = std::min(g_even.leadLo, lead), g_even.leadHi = std::max(g_even.leadHi, lead);
        }
    }
    g_shown.velocity = views ? vr::view_velocity() : vr::Vec3{};
    g_shown.havePivot = views && vr::turn_pivot(g_shown.pivot, g_shown.pivotLimit);
    g_shown.panel = panel;
    g_shown.wheel = wheel;
    g_shown.wrist = g_wristRecorded;
    g_shown.laser = g_laserRecorded;
    g_shown.ui = job.ui;
    g_shown.hands = job.hands && job.handsShow;
    g_shown.uiQuad = job.uiQuad;
    for (int i = 0; i < vr::kWristPanels; ++i) g_shown.wristQuads[i] = g_wrist[i];
    for (int i = 0; i < 2; ++i) g_shown.laserQuads[i] = g_laser[i];
    // timing of this frame: since the last one shown, the render thread's work for it (after xrWaitFrame),
    // the display periods it moved on (more than 1: the runtime showed a frame again, a missed deadline) and
    // the game time it moved on (even motion: the same share of a logic step every frame)
    const auto now = std::chrono::steady_clock::now();
    auto& t = g_times;
    if (t.cpu.size() < 4096) {
        t.cpu.push_back(float(std::chrono::duration<double, std::milli>(now - t.begun).count()));
        if (t.lastEnd.time_since_epoch().count())
            t.interval.push_back(float(std::chrono::duration<double, std::milli>(now - t.lastEnd).count()));
        const XrTime shown = xs().predicted_display_time();
        const XrDuration period = xs().refresh_period();
        if (t.lastDisplay && period > 0) t.display.push_back(float(double(shown - t.lastDisplay) / double(period)));
        if (views) note_display(shown, t.lastDisplay);
        t.lastDisplay = shown;
        g_lastSubmittedDisplay = shown;
        if (gameTime >= 0 && t.lastGame >= 0 && gameTime >= t.lastGame && gameTime - t.lastGame < 4)
            t.game.push_back(float(gameTime - t.lastGame));
        t.lastGame = gameTime;
    }
    t.lastEnd = now;
    if (g_lastGameFrame.time_since_epoch().count()) {
        const double gap = double(std::chrono::duration_cast<std::chrono::nanoseconds>(now - g_lastGameFrame).count());
        if (gap > 0 && gap < 2e8)  // (a load or a stall is not the game's pace)
            g_gameFrameInterval = g_gameFrameInterval > 0 ? g_gameFrameInterval * 0.9 + gap * 0.1 : gap;
    }
    g_lastGameFrame = now;
    {
        const bool was = g_autoLowHeld.load(std::memory_order_relaxed);
        update_auto_low_hold(xs().refresh_rate(), now);
        update_auto_synth_hold(xs().refresh_rate(), xs().has_synthesis_swapchains(), now);
        note_rate_switch(xs().refresh_rate(), now);
        (void)was;
    }
    if (now >= g_boostUntil) xs().cpu_boost(false);  // (headset_frame's Boost over: a no-op when not boosted)
    log_stats();
}

// The headset's frames while the game has none ready (docs/VR_PERFORMANCE.md, "Frames while the game is busy"). The
// headset was given a frame only when the game finished one (frame_begin / frame_end in swap()): while the render
// thread waited for a shader or pipeline the first time it was needed, or idled while the game thread loaded a stage,
// none went to the headset, and the runtime saw the game time out (SteamVR logged 58 to 1,743 a session) and froze or
// greyed the view. Now, once the next frame is half a display period late, the last one is shown again: an OpenXR
// frame whose layers name the same swapchains (their last released images), at the poses they were drawn for, so the
// runtime reprojects them to the head's movement. Nothing is drawn or copied, the game's own frames are not delayed
// (it is never due while one is under way), and the queue is taken only if free (it may be held by a thread that
// waits on the render thread)
// When it is due: the longer of 3 display periods and twice the game's recent frame interval after the last frame shown.
// It was 1.5 periods (16.7 ms at 90 Hz): at 45 fps the game's frames come 2 periods apart, so it showed an old frame
// before nearly every new one (150 to 212 every 10 s, seen in a headset session). Each took a display slot (xrWaitFrame)
// and the next real frame waited a period more, and during a stick turn the old frame showed the world at the last
// turn's angle: the world jerked and swam with head and stick turns (reported in the headset). At 60 fps on a 90 Hz
// headset the frames are 1.5 periods apart, and some went the same way
// Turn smoothing (vr.turnSmoothing; docs/FRAME_INTERPOLATION_IMPLEMENTATION.md, change 3, stage 1), planned: at a frame
// rate under the headset's (Steady 30, 45 fps: each game frame shown for two or three refreshes) and while the view turns
// (vr::view_turn_rate over kTurnMin), each game frame is followed at once by the frames for the refreshes before the next
// one, the same images turned by the view's turn since they were drawn (planned_repeats, right after frame_end in the
// swap). The headset turns every frame for the head itself; the stick's turn it cannot see, so between the game's frames
// the view held still and jumped, and the eyes, following the turn, saw each frame two or three times: the ghosting
// (asked for, 2026-11-12: "why is it smooth when we physically turn our head and not when we move the stick?").
// (The first version, the same day, slipped frames in whenever the render thread was idle: they took the slots the
// game's frames wanted and delayed them, 147 missed refreshes in 10 s of turning at 45 fps. Planned right after the game
// frame, each takes the next refresh before the next game frame asks for one: xrWaitFrame paces them in order.)
constexpr float kTurnMin = 0.05f;  // radians a second (about 3 degrees): slower turns are not shown again
// metres a second (movement smoothing): slower movement is not shown again (0.1 before: a headset run standing still
// measured 0.11, the head's and the animation's noise, and showed 292 frames again every 10 s for nothing)
constexpr float kMoveMin = 0.5f;
bool turning_now() { return std::fabs(g_shown.turnRate) > kTurnMin && vr::config().turnSmoothing; }
bool moving_now() { return vr::length(g_shown.velocity) > kMoveMin && vr::config().movementSmoothing; }
int64_t headset_frame_due() {
    if (!g_started || g_frame || !g_shown.valid || !xs().running()) return 0;
    const XrDuration period = xs().refresh_period();
    if (period <= 0 || g_times.lastEnd.time_since_epoch().count() == 0) return 0;
    const int64_t wait = std::max<int64_t>(3 * int64_t(period), int64_t(2.0 * g_gameFrameInterval));
    const auto due = g_times.lastEnd + std::chrono::nanoseconds(wait);
    return std::chrono::duration_cast<std::chrono::nanoseconds>(due.time_since_epoch()).count();
}
// the last frame shown again: an OpenXR frame of the same images (their last released ones); `motion`: turned (and moved,
// movement smoothing) on by the view's own turn since they were drawn. False: not shown (the queue busy, the session not
// running, no frame begun)
// (docs/VR_STREAMING.md, "Turn smoothing on a streamed headset") on a streamed headset with Streaming optimal (Virtual
// Desktop over Wi-Fi), the runtime's predicted display times move with the network (up to 155 ms apart in a Quest 3
// log): a turned frame's angle taken from them came out uneven, a step too far or too short, and the eyes saw the
// stick-turn ghosting again (reported over VD, 2026-11-13). There each turned frame is turned by whole refreshes from
// the game frame (step 1, 2 ...), the game's own clock; a wired headset keeps the display times.
// WWHD_VR_TURN_STEP_CLOCK=1 or 0 decides for a test start
bool turn_step_clock() {
    static const int forced = [] {
        const char* e = std::getenv("WWHD_VR_TURN_STEP_CLOCK");
        return e && *e == '1' ? 1 : e && *e == '0' ? 0 : -1;
    }();
    const bool on = forced >= 0 ? forced == 1 : (xs().streamed() && vr::config().streamingOptimal);
    static int logged = -1;
    if (int(on) != logged) {
        logged = int(on);
        LOG("[vr] turn smoothing's frames turned by %s", on ? "whole refreshes from the game frame (a streamed headset, "
                                                              "Streaming optimal: its display times move with the network)"
                                                            : "the headset's display times");
    }
    return on;
}
bool show_again(bool motion, int step = 0) {
    end_thread::wait();  // (the last frame's end first: its images released, its xrEndFrame called)
    std::unique_lock<std::mutex> queueLock(graphics_queue_mutex(), std::try_to_lock);
    if (!queueLock) return false;
    xs().poll_events();
    if (!xs().running()) return false;
    bool shouldRender = false;
    if (!xs().begin_frame(shouldRender)) return false;
    const auto rtBegun = std::chrono::steady_clock::now();
    const XrTime rtShown = xs().predicted_display_time();
    const bool rtLate = repeat_late(rtShown);
    // (the views turned by the view's turn since they were drawn: about the head, by the turn rate times the time from
    // their display time to this frame's. The runtime shows an image as seen from the pose it is submitted with; the view's
    // heading in the game being the mapping's heading plus the head's tracked yaw, an image drawn at heading h0 shown at
    // h0 + w dt is the same as one submitted turned by -w dt in the tracking space)
    vr::xr::Views views = g_shown.views;
    if (motion) {
        const XrDuration period = xs().refresh_period();
        const double dt = step > 0 && period > 0 && turn_step_clock()
                              ? double(step) * double(period) * 1e-9
                              : std::clamp(since_shown(xs().predicted_display_time()), -3.0 * double(period) * 1e-9,
                                           6.0 * double(period) * 1e-9);
        const float turn = turning_now() ? float(-double(g_shown.turnRate) * dt) : 0.0f;
        const vr::Quat r = vr::yaw_quat(turn);
        const vr::Vec3 pivot = views.head.position;
        // (movement smoothing) and moved back by the view's movement since: an image drawn with the view at p0, shown with
        // it at p0 + v dt, is the same as one submitted from where the head was less v dt in the tracking space; the
        // runtime moves it by the depth sent with it (reuse_last_depth)
        const vr::Vec3 shift = moving_now() ? g_shown.velocity * float(-dt) : vr::Vec3{};
        views.head.orientation = vr::normalize(r * views.head.orientation);
        views.head.position = pivot + shift;
        for (auto& e : views.eye) {
            e.pose.position = pivot + vr::rotate(r, e.pose.position - pivot) + shift;
            e.pose.orientation = vr::normalize(r * e.pose.orientation);
        }
        if (moving_now()) xs().reuse_last_depth();
    }
    // (the game's UI layer too: its last released image, as the eyes'. Left out, the HUD, text and menus of the per-eye
    // projection's UI layer blinked off for every frame shown again: loads, shader waits)
    vr::xr::Quad panel = g_shown.panel, wheel = g_shown.wheel, ui = g_shown.uiQuad;
    // (docs/VR_HANDS_LAYER_PLAN.md step 4) the hands layer at the views the frame was drawn with, not turned: the eyes'
    // layer turns with the stick, the hands stay where they were (Link's hands follow the controllers, not the turn)
    xs().end_frame(views, g_shown.drawn, &panel, &wheel, g_shown.wrist ? g_shown.wristQuads : nullptr,
                   g_shown.wrist ? vr::kWristPanels : 0, g_shown.laser ? g_shown.laserQuads : nullptr, g_shown.laser ? 2 : 0,
                   g_shown.ui ? &ui : nullptr, g_shown.hands ? &g_shown.views : nullptr);
    if (motion) {
        g_rtPlain.add(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - rtBegun).count(), rtLate);
        g_lastSubmittedDisplay = rtShown;
        repeat_timing_log();
    }
    g_times.lastEnd = std::chrono::steady_clock::now();
    return true;
}
void headset_frame() {
    const int64_t due = headset_frame_due();
    const auto now = std::chrono::steady_clock::now();
    if (!due || std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count() < due) return;
    if (!show_again(false)) return;
    ++g_stats.keptAlive;
    // (standalone: the CPU at Boost while the game is stalled, Sustained High again 2 s after the last stall: frame_end)
    g_boostUntil = g_times.lastEnd + std::chrono::seconds(2);
    xs().cpu_boost(true);
}
// A generated frame (docs/FRAME_INTERPOLATION_IMPLEMENTATION.md; the "Generated frames" setting itself was removed,
// 2026-11-13: "it doesn't work anyway"; used by Head movement smoothing and Turn smoothing's near-field hold): one of the
// refreshes planned_repeats fills, a frame drawn on the GPU from the last real one (vr_genframe): both eyes' pictures
// reprojected by their scene depth (vr_framesynth::last_depth) to the head where the runtime predicts it for this
// refresh and the view's own turn and movement since (an image drawn at heading h0 seen at h0 + w dt, from p0 + v dt:
// the camera turned by w dt about the head and moved by v dt in the tracking space), submitted at the head's own pose.
// The sequence is the headset's test loop's (smoke): the frame begun, the eyes located, both images acquired, drawn,
// submitted, then released and the frame ended under the queue's lock. -1: no frame begun (nothing to generate from: the
// caller shows the last one again turned instead); 0: begun but shown as the last one (a draw failed); 1: generated
constexpr float kGenMoveMin = 0.3f;
// (diagnostics, every 10 s with any generated frame: "[vr] generated frames (10 s)") what each generated frame was moved
// by and where it landed: its time after the real frame it was made from, the turn and the movement drawn into it, and
// its refresh against the frame before it (one refresh after it: on its slot; more: a refresh was missed; less: two
// frames for one refresh)
struct GenStats {
    uint64_t frames = 0, onSlot = 0, late = 0, early = 0;
    float nearM = 0, farM = 0;           // (check 5) the last frame's planes in metres
    double fovAspect = 0, picAspect = 0;  // (check 6) the left eye's field of view (tangents) and its picture, width over height
    double dtSum = 0, dtMax = 0, turnSum = 0, turnMax = 0, moveSum = 0, moveMax = 0;
    uint64_t headFrames = 0;            // (head movement) frames warped by the head's own movement only
    double headSum = 0, headMax = 0;    // the eyes' movement since the real frame, mm
    std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
} g_gen;  // metres a second: slower movement (with no turn) is not worth a generated frame
// (diagnostics, WWHD_VR_GENFRAME_CAPTURE=1, 2026-11-12, asked for: "add the capture") the left eye of a stick turn, as
// the headset gets it: the real frame, the generated frames after it, the next real frame, as PNGs in
// captures/genframe/ (set N: a_real, b_gen1, c_gen2, d_next_real) with a text of what each was drawn with. A set while
// turning faster than 15 degrees a second, every 4 s, 4 sets at most. Each image waits for the GPU (a hitch)
struct GenCapture {
    bool on = false;
    int sets = 0;
    int inSet = -1;    // the set being taken (-1: none)
    int gens = 0;      // its generated frames so far
    bool wantNext = false;
    std::chrono::steady_clock::time_point last{};
    std::string notes;
} g_cap;
std::string capture_dir() {
    std::string d = "captures/genframe";
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    return d;
}
void capture_write(std::string path, uint32_t w, uint32_t h, std::vector<uint8_t> rgba) {
    std::thread([path = std::move(path), w, h, rgba = std::move(rgba)] {
        try {
            write_rgba_png(path, w, h, rgba);
        } catch (...) {
        }
    }).detach();
}
// the real left-eye picture as drawn (its shown part), display-encoded
void capture_real(const char* name) {
    Screen& eye = R.tv;
    if (!eye.scan || !eye.scan->image) return;
    end_thread::wait();  // (the last frame's end first: its images released)
    Surface& scan = *eye.scan;
    std::vector<uint8_t> full;
    try {
        full = read_surface_rgba(scan, true);
    } catch (...) {
        return;
    }
    const VkExtent3D shown = scan_picture_extent(eye);
    const uint32_t fw = scan.extent.width, sw = shown.width ? std::min(shown.width, fw) : fw;
    const uint32_t sh = shown.height ? std::min(shown.height, scan.extent.height) : scan.extent.height;
    std::vector<uint8_t> crop(size_t(sw) * sh * 4);
    for (uint32_t y = 0; y < sh; ++y) std::memcpy(&crop[size_t(y) * sw * 4], &full[size_t(y) * fw * 4], size_t(sw) * 4);
    char path[256];
    std::snprintf(path, sizeof path, "%s/set%d_%s.png", capture_dir().c_str(), g_cap.inSet, name);
    capture_write(path, sw, sh, std::move(crop));
    char line[256];
    std::snprintf(line, sizeof line, "%s: the real picture, %ux%u (the eye's drawn part)\n", name, sw, sh);
    g_cap.notes += line;
}
void capture_notes() {
    char path[256];
    std::snprintf(path, sizeof path, "%s/set%d.txt", capture_dir().c_str(), g_cap.inSet);
    if (FILE* f = std::fopen(path, "w")) {
        std::fputs(g_cap.notes.c_str(), f);
        std::fclose(f);
    }
    LOG("[vr] generated frames: capture set %d written (%s)", g_cap.inSet, path);
}
// Head movement smoothing (vr.headSmoothing, 2026-11-13, asked for: "make b with head movements", its own setting under
// Streaming optimal): the head moves the eyes a few centimetres as you look around, and the runtime's reprojection of a
// repeated refresh corrects only the head's turn, so near things (the arms, the sword, close walls) stepped back and forth
// at the game's rate (players at 45 and 36 fps, wired and streamed; a real 90 was clean). Each refresh between the game's
// frames with no stick turn and no movement of the view gets the last frame warped by the eyes' movement alone
// (show_generated headMode). Separate from Generated frames (off: "didn't work for us")
bool head_smoothing_wanted() {
    return vr::config().headSmoothing && g_shown.drawn && g_shown.views.valid && !turning_now() && !moving_now();
}
// turnOnly (Turn smoothing, 2026-11-12, asked for: "for turn smoothing lets fix the body or near field objects"): the
// frame is Turn smoothing's turned copy (the stick's turn in the submitted pose, the far world left to the runtime, as
// show_again), with only the near field (the body, hands, sword, shield) warped to stay where it was; no walk
int show_generated(bool turnOnly, bool headMode = false) {
    Screen* eyes[2] = {&R.tv, &stereoRight};
    // (Head movement smoothing, 2026-11-13) the frame warped by the eyes' movement since it was drawn only (the parallax the
    // runtime cannot add), its turn left to the runtime: submitted with the real frame's orientations at the eyes' positions
    // now. The runtime then turns it to the head as it does every frame
    const bool headOnly = headMode && !turnOnly;
    Surface* depth[2] = {};
    uint32_t slice[2] = {};
    float projA[2] = {}, projB[2] = {};
    bool halfZ[2] = {};
    for (int e = 0; e < 2; ++e) {
        if (!eyes[e]->scan || !eyes[e]->scan->image) return -1;
        if (!vr_framesynth::last_depth(e, depth[e], slice[e], projA[e], projB[e], halfZ[e])) return -1;
    }
    const float scale = vr::world_scale();
    if (!(scale > 0) || !vr_genframe::prepare(VkFormat(xs().swapchain(0).format)) ||
        !vr_genframe::prepare(VkFormat(xs().swapchain(1).format)))
        return -1;
    end_thread::wait();  // (the last frame's end first: its images released, its xrEndFrame called)
    xs().poll_events();
    if (!xs().running()) return -1;
    bool shouldRender = false;
    if (!xs().begin_frame(shouldRender)) return -1;
    const auto rtBegun = std::chrono::steady_clock::now();
    const bool rtLate = repeat_late(xs().predicted_display_time());
    vr::xr::Views out = g_shown.views;
    bool drawn = false;
    vr::xr::Views located;
    if (shouldRender && xs().locate_views(xs().predicted_display_time(), located)) {
        float eyeX[2];
        const vr::xr::Views q = vr::as_rendered(located, eyeX);
        const XrDuration period = xs().refresh_period();
        const double dt = std::clamp(since_shown(xs().predicted_display_time()), -3.0 * double(period) * 1e-9, 6.0 * double(period) * 1e-9);
        // (check 3, WWHD_VR_GENFRAME_TURN_SCALE=x: the turn drawn into generated frames times x, so its direction shows: at 3
        // they jump ahead in the turn's direction when it is right, back against it when it is wrong)
        static const double turnScale = [] {
            const char* e = std::getenv("WWHD_VR_GENFRAME_TURN_SCALE");
            const double v = e ? std::atof(e) : 1.0;
            return v > 0 && v < 10 ? v : 1.0;
        }();
        const vr::Quat turn = vr::yaw_quat(float(double(g_shown.turnRate) * dt * turnScale));
        if (g_shown.views.hasHeading)
            note_shown(float(double(g_shown.views.heading) + double(g_shown.turnRate) * dt * turnScale), xs().predicted_display_time(),
                       true, g_shown.turnRate);
        const vr::Vec3 move = !turnOnly && vr::length(g_shown.velocity) > kGenMoveMin ? g_shown.velocity * float(dt) : vr::Vec3{};
        {
            const XrTime shownAt = xs().predicted_display_time();
            const double turnDeg = std::fabs(double(g_shown.turnRate) * dt) * 57.29578, moveCm = double(vr::length(move)) * 100.0;
            ++g_gen.frames;
            g_gen.dtSum += dt * 1000.0, g_gen.dtMax = std::max(g_gen.dtMax, dt * 1000.0);
            g_gen.turnSum += turnDeg, g_gen.turnMax = std::max(g_gen.turnMax, turnDeg);
            g_gen.moveSum += moveCm, g_gen.moveMax = std::max(g_gen.moveMax, moveCm);
            if (g_lastSubmittedDisplay && period > 0) {
                const double gap = double(shownAt - g_lastSubmittedDisplay) / double(period);
                if (gap > 1.5) ++g_gen.late;
                else if (gap < 0.5) ++g_gen.early;
                else ++g_gen.onSlot;
            }
            g_lastSubmittedDisplay = shownAt;
            const auto now = std::chrono::steady_clock::now();
            if (now - g_gen.since >= std::chrono::seconds(10)) {
                const double n = double(std::max<uint64_t>(g_gen.frames, 1));
                LOG("[vr] generated frames (10 s): %llu; %.1f ms after their real frame on average (%.1f at most); turned %.2f "
                    "degrees on average (%.2f at most), moved %.1f cm (%.1f at most); refreshes: %llu on their slot, %llu a "
                    "refresh late, %llu sharing one; depth planes %.3f to %.0f m; field of view %.3f wide for 1 high, the "
                    "picture %.3f (%s); %llu by the head's movement only (the eyes moved %.1f mm on average, %.1f at most)",
                    (unsigned long long)g_gen.frames, g_gen.dtSum / n, g_gen.dtMax,
                    g_gen.turnSum / n, g_gen.turnMax, g_gen.moveSum / n, g_gen.moveMax, (unsigned long long)g_gen.onSlot,
                    (unsigned long long)g_gen.late, (unsigned long long)g_gen.early, double(g_gen.nearM), double(g_gen.farM),
                    g_gen.fovAspect, g_gen.picAspect,
                    "the eye's own field of view; its pixels need not be square", (unsigned long long)g_gen.headFrames,
                    g_gen.headFrames ? g_gen.headSum / double(g_gen.headFrames) : 0.0, g_gen.headMax);
                g_gen = GenStats{};
            }
        }
        VkImage img[2] = {xs().acquire(0), VK_NULL_HANDLE};
        if (img[0]) img[1] = xs().acquire(1);
        if (img[0] && img[1]) {
            auto cmd = command_buffer();
            end_encoder();
            bool both = true;
            // (2026-11-12, "pose" mode, the default: asked for after a headset run showed Turn smoothing's turned copies
            // right for far objects, the body shaking) the stick's turn submitted in the pose, as Turn smoothing's copies
            // (show_again): the real frame's views turned by -w dt about the head, the runtime turning the picture for it
            // as it does for the head. The far world is not warped (left to the runtime: the method seen working); only
            // the near field (the body, hands, sword, shield) is warped, to the submitted pose with it held in the
            // tracking space, so the runtime's turn leaves it where it was. WWHD_VR_GENFRAME_MODE=warp: the warp turns the
            // world as before (submitted at the head's pose)
            static const bool poseModeSetting = [] {
                const char* m = std::getenv("WWHD_VR_GENFRAME_MODE");
                const bool pose = !(m && !std::strcmp(m, "warp"));
                LOG("[vr] generated frames: %s", pose ? "the stick's turn in the submitted pose (as Turn smoothing), the near "
                                                        "field held (WWHD_VR_GENFRAME_MODE=warp: the world warped)"
                                                      : "the world warped by the stick's turn, submitted at the head's pose");
                return pose;
            }();
            const bool poseMode = poseModeSetting || turnOnly;
            vr::xr::Views turned = g_shown.views;  // (pose mode) the views submitted: the real frame's, turned
            if (poseMode) {
                const vr::Quat r = vr::yaw_quat(float(-double(g_shown.turnRate) * dt * turnScale));
                const vr::Vec3 pivot = turned.head.position;
                turned.head.orientation = vr::normalize(r * turned.head.orientation);
                for (auto& ev : turned.eye) {
                    ev.pose.position = pivot + vr::rotate(r, ev.pose.position - pivot);
                    ev.pose.orientation = vr::normalize(r * ev.pose.orientation);
                }
            }
            for (int e = 0; e < 2; ++e) {
                const vr::Pose& s = g_shown.views.eye[e].pose;  // the real eye camera
                if (headOnly) {
                    // the target camera: the real one's orientation at the eye's position now (in the real camera's space)
                    const vr::Vec3 o = vr::rotate(vr::conjugate(s.orientation), q.eye[e].pose.position - s.position);
                    const vr::Fov& f = g_shown.views.eye[e].fov;
                    vr_genframe::Warp w{};
                    w.axisX[0] = 1, w.axisY[1] = 1, w.axisZ[2] = 1;
                    w.origin[0] = o.x, w.origin[1] = o.y, w.origin[2] = o.z;
                    w.nearRotation[3] = 1;  // (the near field warped as the world: the same camera)
                    w.nearOrigin[0] = o.x, w.nearOrigin[1] = o.y, w.nearOrigin[2] = o.z;
                    w.nearMetres = 0;
                    w.tanLeft = std::tan(f.left), w.tanRight = std::tan(f.right), w.tanUp = std::tan(f.up), w.tanDown = std::tan(f.down);
                    w.projA = projA[e], w.projB = projB[e] / scale, w.halfZ = halfZ[e];
                    if (e == 0) {
                        const double mm = double(vr::length(q.eye[e].pose.position - s.position)) * 1000.0;
                        ++g_gen.headFrames;
                        g_gen.headSum += mm, g_gen.headMax = std::max(g_gen.headMax, mm);
                    }
                    Surface* scan = eyes[e]->scan.get();
                    const VkExtent3D shown = scan_picture_extent(*eyes[e]);
                    const auto& sc = xs().swapchain(e);
                    both = both && vr_genframe::draw(cmd, *scan, shown.width ? shown.width : scan->extent.width,
                                                      shown.height ? shown.height : scan->extent.height, *depth[e], slice[e],
                                                      w, img[e], VkFormat(sc.format), sc.width, sc.height);
                    out.eye[e].pose.orientation = s.orientation;
                    out.eye[e].pose.position = q.eye[e].pose.position;
                    out.eye[e].rectWidth = out.eye[e].rectHeight = 0;  // (the whole image written)
                    continue;
                }
                const vr::Quat tOrient = vr::normalize(turn * q.eye[e].pose.orientation);
                // (2026-11-12) turned about the game's own pivot: the head's offset from the room-scale origin (held within
                // the limit sideways) swings with the turn, as the game turns the mapping about that origin; about the head,
                // standing a metre from the origin put every generated frame up to 4 cm off sideways during a stick turn
                vr::Vec3 headT = q.head.position;
                if (g_shown.havePivot) {
                    vr::Vec3 off = q.head.position - g_shown.pivot;
                    off.y = 0;
                    const float h = vr::length(off), lim = g_shown.pivotLimit;
                    if (lim >= 0 && h > lim && h > 0) off = off * (lim / h);
                    headT = q.head.position + vr::rotate(turn, off) - off;
                }
                const vr::Vec3 tPos = headT + vr::rotate(turn, q.eye[e].pose.position - q.head.position) + move;
                const vr::Quat rel = poseMode ? vr::Quat{} : vr::normalize(vr::conjugate(s.orientation) * tOrient);
                const vr::Vec3 ax = vr::rotate(rel, {1, 0, 0}), ay = vr::rotate(rel, {0, 1, 0}), az = vr::rotate(rel, {0, 0, 1});
                // (pose mode: the world only moved by the walk, in the real camera's space; its turn is the pose's)
                const vr::Vec3 o = poseMode ? vr::rotate(vr::conjugate(s.orientation), move)
                                            : vr::rotate(vr::conjugate(s.orientation), tPos - s.position);
                const vr::Fov& f = g_shown.views.eye[e].fov;
                vr_genframe::Warp w{};
                w.axisX[0] = ax.x, w.axisX[1] = ax.y, w.axisX[2] = ax.z;
                w.axisY[0] = ay.x, w.axisY[1] = ay.y, w.axisY[2] = ay.z;
                w.axisZ[0] = az.x, w.axisZ[1] = az.y, w.axisZ[2] = az.z;
                w.origin[0] = o.x, w.origin[1] = o.y, w.origin[2] = o.z;
                // (the near field: the hands, sword and shield move with you) its camera: the eye where the head is now,
                // without the stick's turn or the walk (WWHD_VR_GENFRAME_NEAR=metres, default 1; 0: off)
                {
                    static const float nearMetres = [] {
                        const char* e = std::getenv("WWHD_VR_GENFRAME_NEAR");
                        const float m = e ? float(std::atof(e)) : 1.0f;
                        const float r = std::isfinite(m) ? std::clamp(m, 0.0f, 3.0f) : 1.0f;
                        LOG("[vr] generated frames: the near field (within %.2f m: hands, sword, shield) moves with the head only%s",
                            double(r), r > 0 ? "" : " (off: warped as the world)");
                        return r;
                    }();
                    // (pose mode: to the submitted, turned pose, the near field held in the tracking space; else to the head
                    // where it is now)
                    const vr::Pose& nt = poseMode ? turned.eye[e].pose : q.eye[e].pose;
                    const vr::Quat nr = vr::normalize(vr::conjugate(s.orientation) * nt.orientation);
                    const vr::Vec3 no = vr::rotate(vr::conjugate(s.orientation), nt.position - s.position);
                    w.nearRotation[0] = nr.x, w.nearRotation[1] = nr.y, w.nearRotation[2] = nr.z, w.nearRotation[3] = nr.w;
                    w.nearOrigin[0] = no.x, w.nearOrigin[1] = no.y, w.nearOrigin[2] = no.z;
                    w.nearMetres = nearMetres;
                    if (poseMode && nearMetres > 0) w.nearMetres = -nearMetres;  // (the world still: vr_genframe worldStill)
                }
                w.tanLeft = std::tan(f.left), w.tanRight = std::tan(f.right), w.tanUp = std::tan(f.up), w.tanDown = std::tan(f.down);
                // (the views' field of view: the one each eye's projection is built from, as_rendered. Not the camera
                // matrix noted for depth (vr_framesynth last_tangents): that is the game's own, before the eye's frustum
                // (stereo_projection.h project_eye), and a headset run with it read 1.386 wide for 1 high against the
                // eye's 1.049, symmetric: every generated frame turned by about three quarters of the turn, 2026-11-12.
                // The picture's pixels need not be square, so its width over height is no check of this)
                w.projA = projA[e], w.projB = projB[e] / scale, w.halfZ = halfZ[e];
                // (checks 5 and 6, logged with the generated frames' line) the planes in metres and the field of view
                // against the picture's shape
                if (e == 0) {
                    const float a = projA[e], b = projB[e] / scale;
                    g_gen.nearM = halfZ[e] ? b / a : b / (a - 1.0f);
                    g_gen.farM = a == -1.0f ? INFINITY : b / (a + 1.0f);
                    const VkExtent3D sh = scan_picture_extent(*eyes[e]);
                    const double pw = sh.width ? sh.width : eyes[e]->scan->extent.width, ph = sh.height ? sh.height : eyes[e]->scan->extent.height;
                    g_gen.fovAspect = (w.tanRight - w.tanLeft) / std::max(1e-6f, w.tanUp - w.tanDown);
                    g_gen.picAspect = ph > 0 ? pw / ph : 0;
                }
                Surface* scan = eyes[e]->scan.get();
                const VkExtent3D shown = scan_picture_extent(*eyes[e]);
                const auto& sc = xs().swapchain(e);
                both = both && vr_genframe::draw(cmd, *scan, shown.width ? shown.width : scan->extent.width,
                                                  shown.height ? shown.height : scan->extent.height, *depth[e], slice[e], w,
                                                  img[e], VkFormat(sc.format), sc.width, sc.height);
                // (submitted at the head's own pose: the view's turn and move are drawn in; pose mode: the turned views)
                out.eye[e].pose = poseMode ? turned.eye[e].pose : q.eye[e].pose;
                out.eye[e].rectWidth = out.eye[e].rectHeight = 0;  // (the whole image written)
            }
            out.head = headOnly ? vr::Pose{g_shown.views.head.orientation, q.head.position} : poseMode ? turned.head : q.head;
            if (both && seq_capture_on() && g_seq.frame >= 0 && g_seq.frame < SeqCapture::kFrames) {
                for (int e = 0; e < 2; ++e) {
                    seq_copy(cmd, g_seq.frame, e, img[e]);
                    seq_note(g_seq.frame, e, 1, xs().predicted_display_time(), out.eye[e].pose, out.eye[e].fov, g_shown.turnRate,
                             (double(g_shown.views.heading) + double(g_shown.turnRate) * dt * turnScale) * 57.29578, dt * 1000.0);
                }
                if (++g_seq.frame == SeqCapture::kFrames) seq_finish();
            }
            if (both && g_cap.inSet >= 0 && !g_cap.wantNext) {
                std::vector<uint8_t> rgba;
                const auto& sc = xs().swapchain(0);
                if (vr_genframe::read_target(img[0], VkFormat(sc.format), sc.width, sc.height, rgba)) {
                    ++g_cap.gens;
                    char path[256], line[512];
                    std::snprintf(path, sizeof path, "%s/set%d_%c_gen%d.png", capture_dir().c_str(), g_cap.inSet,
                                  char('a' + g_cap.gens), g_cap.gens);
                    capture_write(path, sc.width, sc.height, std::move(rgba));
                    const vr::Fov& f = g_shown.views.eye[0].fov;
                    std::snprintf(line, sizeof line,
                                  "gen%d: %ux%u, %.1f ms after the real frame, turned %.3f degrees (rate %.1f deg/s), moved %.1f "
                                  "cm; field of view tangents left %.3f right %.3f up %.3f down %.3f\n",
                                  g_cap.gens, sc.width, sc.height, dt * 1000.0, double(g_shown.turnRate) * dt * 57.29578,
                                  double(g_shown.turnRate) * 57.29578, double(vr::length(move)) * 100.0, std::tan(f.left),
                                  std::tan(f.right), std::tan(f.up), std::tan(f.down));
                    g_cap.notes += line;
                }
            }
            flush_async();
            drawn = both;
        }
        std::lock_guard<std::mutex> queueLock(graphics_queue_mutex());
        if (img[0]) xs().release(0);
        if (img[1]) xs().release(1);
        if (!drawn) out = g_shown.views;  // (a draw failed: the images hold what they held; the old frame's views)
    }
    // (Head movement smoothing with Depth for Virtual Desktop, 2026-11-13, asked for: "fix it so they work together") the
    // real frame's depth sent with its warped frames too, as movement smoothing's repeats (its depth swapchains' last
    // released images): VDXR places the layers by depth, and depth on every other refresh only (the real frames) could
    // flicker what it places by it. Drawn a few millimetres from where the warped frame is shown: close enough
    if (headOnly && drawn) xs().reuse_last_depth();
    {
        vr::xr::Quad panel = g_shown.panel, wheel = g_shown.wheel, ui = g_shown.uiQuad;
        std::lock_guard<std::mutex> queueLock(graphics_queue_mutex());
        xs().end_frame(out, drawn || g_shown.drawn, &panel, &wheel, g_shown.wrist ? g_shown.wristQuads : nullptr,
                       g_shown.wrist ? vr::kWristPanels : 0, g_shown.laser ? g_shown.laserQuads : nullptr,
                       g_shown.laser ? 2 : 0, g_shown.ui ? &ui : nullptr, g_shown.hands ? &g_shown.views : nullptr);
    }
    if (turnOnly) {
        g_rtHeld.add(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - rtBegun).count(), rtLate);
        repeat_timing_log();
    }
    g_times.lastEnd = std::chrono::steady_clock::now();
    return drawn ? 1 : 0;
}
bool hands_layer_wanted() {
    // (a test start's WWHD_VR_FRAME_RATE wins, as for the game's pacing: a Capped 45 test run had the layer off, 2026-11-13)
    static const int testRate = [] {
        const char* e = std::getenv("WWHD_VR_FRAME_RATE");
        if (!e || !*e) return -1;
        return !std::strcmp(e, "full") ? int(vr::FrameRate::Full) : !std::strcmp(e, "half") ? int(vr::FrameRate::Half)
             : !std::strcmp(e, "auto") ? int(vr::FrameRate::Auto) : !std::strcmp(e, "unlocked") ? int(vr::FrameRate::Unlocked) : -1;
    }();
    // (the setting, vr.handsLayer, with Turn smoothing; WWHD_VR_HANDS_LAYER=1 or 0 decides for a test start)
    static const int forced = [] {
        const char* e = std::getenv("WWHD_VR_HANDS_LAYER");
        return e && *e == '1' ? 1 : e && *e == '0' ? 0 : -1;
    }();
    const vr::Config c = vr::config();
    if (forced == 0 || (forced < 0 && !(c.handsLayer && c.turnSmoothing))) return false;
    const vr::FrameRate mode = testRate >= 0 ? vr::FrameRate(testRate) : c.frameRate;
    // (2026-11-14, a headset log in Auto Mode: while Auto aimed for 90 the game drew 77 fps, the in-between passes over
    // their time, the GPU 32 % busy, and steady hands was on in 409 of its frames: on at each dip under 90 (the measured
    // pace, update_auto_low_hold), it added Link's own image, depth and layer just when the game was short of time.
    // Asked for, 2026-11-12: "make sure we don't use this in 90 fps modes so our fps isn't impacted") Auto Mode: on by the
    // rate Auto has chosen, as the refresh filling is (planned_repeats): on once Auto has gone to half the headset's rate
    // (45 at 90 Hz), off while it aims for the full rate
    if (mode == vr::FrameRate::Auto) {
        const double hz = xs().refresh_rate();
        const int fps = interp::mode() == 1 ? interp::output_fps() : 0;
        return g_started && hz > 0 && fps > 0 && double(fps) * 1.5 <= hz;
    }
    return g_started && (mode == vr::FrameRate::Half || mode == vr::FrameRate::Original);
}
// (the log: refreshes filled with the last frame again, not turned or moved, per 10 s)
uint64_t& plain_fills() { static uint64_t n = 0; return n; }
void planned_repeats() {
    if (!g_started || !g_shown.valid || !g_shown.drawn || !g_shown.views.valid || !xs().running()) return;
    // only in the low frame-rate modes (Capped 45, Steady 30: vr.frameRate half or original), never in Capped 90,
    // Unlocked or Auto (asked for, 2026-11-12: "make sure we don't use this in 90 fps modes so our fps isn't impacted")
    // (and Auto Mode while the game runs below the headset's rate: low_rate_repeats)
    const double hz = xs().refresh_rate();
    const int fps = interp::mode() == 1 ? interp::output_fps() : 30;
    // (2026-11-14, a headset log in Auto Mode: the game held at 22.4-22.5 fps with interpolation aiming for 45 and 90, "3 a
    // frame" filled, the GPU 5-11 % busy) In Auto Mode the count was the game's measured frame time (low_rate_repeats):
    // each fill takes a refresh the render thread waits for, so 3 fills let the game start a frame only every 4th refresh,
    // which measured as slow again: a trap from any slow patch (a stage loading). Now Auto Mode fills as Capped does, by
    // the rate Auto has chosen (its interpolation's output: 90 none, 45 one a frame), never more than that gap. The
    // measured pace still decides Auto's "below the headset's rate" (update_auto_low_hold, steady hands)
    const int repeats = vr::config().frameRate == vr::FrameRate::Auto
                            ? (hz > 0 && fps > 0 ? std::clamp(int(std::lround(hz / double(fps))) - 1, 0, 3) : 0)
                            : low_rate_repeats(hz, fps);
    if (repeats <= 0) return;
    // (docs/VR_MOVING_OBJECTS_PLAN.md, "Separate small fix", 2026-11-14) not while the runtime's frame synthesis makes the
    // in-between frames: the frame just shown carried both eyes' motion vectors (a Quest at 72 Hz, Capped 30 drawn at 36;
    // Capped 45 at 90 Hz). Frames sent here carry none and would take the refreshes the runtime synthesizes, against its
    // half-rate pacing. Depth only (PC SteamVR) never carries: its fills go on. Steady 30 at 90 or 120 Hz holds frame
    // synthesis (vr_framesynth::set_held, with vr.frameSynthesis auto or on: the chosen 30 is kept, never raised to 45),
    // so its frames carry nothing and its fills go on. The log: the first decision and each change, with the reason
    {
        static int paused = -1;
        static uint64_t pausedFrames = 0, filledFrames = 0;
        const bool synth = vr_framesynth::carrying();
        if (int(synth) != paused) {
            paused = int(synth);
            LOG("[vr] refresh filling: %s (%.0f Hz, %d refreshes filled a game frame; since the start: %llu game frames "
                "paused, %llu with fills on)",
                synth ? "paused (the runtime's frame synthesis makes the in-between frames)"
                : vr_framesynth::held() ? "on (frame synthesis held: the game under half the refresh rate, Steady 30 or "
                                          "Auto Mode running slow; not raised to 45)"
                                        : "on (no frame synthesis)",
                hz, repeats, (unsigned long long)pausedFrames, (unsigned long long)filledFrames);
        }
        ++(synth ? pausedFrames : filledFrames);
        if (synth) return;
    }
    const bool headSmooth = head_smoothing_wanted();
    {
        static const bool capture = [] {
            const char* e = std::getenv("WWHD_VR_GENFRAME_CAPTURE");
            return e && *e == '1';
        }();
        g_cap.on = capture;
        if (g_cap.on && g_cap.wantNext) {  // (the set's last picture: the next real frame)
            capture_real("d_next_real");
            capture_notes();
            g_cap.inSet = -1, g_cap.wantNext = false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (g_cap.on && turning_now() && g_cap.inSet < 0 && g_cap.sets < 4 && std::fabs(g_shown.turnRate) > 15.0f / 57.29578f &&
            now - g_cap.last > std::chrono::seconds(4)) {
            g_cap.inSet = ++g_cap.sets, g_cap.gens = 0, g_cap.last = now;
            char line[256];
            std::snprintf(line, sizeof line, "set %d: the stick turning at %.1f degrees a second (heading +: left)\n",
                          g_cap.inSet, double(g_shown.turnRate) * 57.29578);
            g_cap.notes = line;
            capture_real("a_real");
        }
    }
    // (2026-11-14, in the headset at Steady 30: "screen looks very jittery and jumpy when i move around or look around") Not
    // turning or moving, the refreshes between the game's frames were left empty: the headset got 30 frames a second, and
    // SteamVR's Motion Smoothing filled them (on in 96-98% of frames, switching on and off 8-11 times every 10 s in that
    // run's log; the owner: "motion smoothing in steamvr is always worse for me and any tester that uses it"). Now every
    // refresh gets a frame: the last game frame again at the views it was drawn with (show_again: nothing drawn or copied,
    // the runtime turns it to the head as it does every frame), so the runtime never sees a slow app.
    // WWHD_VR_FILL_REFRESHES=0 leaves them empty as before (a test start)
    static const bool fill = [] {
        const char* e = std::getenv("WWHD_VR_FILL_REFRESHES");
        const bool on = !(e && *e == '0');
        LOG("[vr] the refreshes between the game's frames at 30/45 fps: %s", on ? "each given the last frame again (the "
            "runtime's Motion Smoothing not needed; WWHD_VR_FILL_REFRESHES=0: left empty)" : "left empty (WWHD_VR_FILL_REFRESHES=0)");
        return on;
    }();
    if (!(headSmooth || turning_now() || moving_now() || fill)) {
        if (g_cap.inSet >= 0 && !g_cap.wantNext) g_cap.inSet = -1;  // (the turn stopped before its frames)
        return;
    }
    const bool motion = turning_now() || moving_now();
    int shown = 0;
    static uint64_t generated = 0, generationFailed = 0;
    static float maxTurn = 0, maxMove = 0;
    static uint64_t& g_plainFills = plain_fills();
    maxTurn = std::max(maxTurn, std::fabs(g_shown.turnRate));
    maxMove = std::max(maxMove, vr::length(g_shown.velocity));
    for (int i = 0; i < repeats; ++i) {
        if (headSmooth) {  // (Head movement smoothing: the eyes' movement since the real frame, warped by its depth)
            const int g = show_generated(false, true);
            if (g == 1) {
                ++generated;
                ++shown;
                continue;
            }
            if (g == 0) {
                ++generationFailed;
                ++shown;
                continue;
            }
        }
        // (Turn smoothing: the turned copy with the near field held, by the scene's depth; WWHD_VR_TURN_NEAR_HOLD=0: the
        // plain turned copy, the body turning with the world)
        static const bool nearHold = [] {
            const char* e = std::getenv("WWHD_VR_TURN_NEAR_HOLD");
            // (off by default since a headset run the same day: "everything is worse now, we made the far view not work";
            // WWHD_VR_TURN_NEAR_HOLD=1 tries it)
            const bool on = e && *e == '1';
            LOG("[vr] turn smoothing: %s", on ? "the body, hands, sword and shield held in its frames (WWHD_VR_TURN_NEAR_HOLD=0: "
                                                "turned with the world)" : "the whole picture turned (the near field not held)");
            return on;
        }();
        if (nearHold && turning_now()) {
            const int g = show_generated(true);
            if (g == 1) {
                ++generated;
                ++shown;
                continue;
            }
            if (g == 0) {
                ++generationFailed;
                ++shown;
                continue;
            }
        }
        if (!show_again(motion, i + 1)) break;
        ++shown;
        if (!motion && !headSmooth) ++g_plainFills;
    }
    if (g_cap.inSet >= 0 && !g_cap.wantNext) g_cap.wantNext = true;
    // (the log: frames shown again turned, every 10 s with any)
    static uint64_t turned = 0, frames = 0;
    static auto since = std::chrono::steady_clock::now();
    turned += uint64_t(shown);
    frames += 1;
    const auto now = std::chrono::steady_clock::now();
    if (now - since >= std::chrono::seconds(10)) {
        LOG("[vr] turn smoothing (10 s): %llu frames between %llu game frames (%d a frame; %llu generated, %llu generations "
            "failed, the rest turned copies, %llu of them plain fills with no turn or movement; the view turning at %.0f "
            "degrees/s at most, moving at %.2f m/s at most)",
            (unsigned long long)turned, (unsigned long long)frames, repeats, (unsigned long long)generated,
            (unsigned long long)generationFailed, (unsigned long long)g_plainFills, double(maxTurn) * 57.29578, double(maxMove));
        turned = frames = 0;
        g_plainFills = 0;
        generated = generationFailed = 0;
        maxTurn = maxMove = 0;
        since = now;
    }
}

void shutdown() {
    if (xs().initialised()) stop("");
}

int smoke(int frames) {
    if (!xs().initialised()) {
        LOG("[vr smoke] skipped: %s", vr::status().empty() ? "VR not wanted (WWHD_VR=1 or --vr)" : vr::status().c_str());
        return 0;
    }
    // the runtime starts the session when the headset is ready (READY -> xrBeginSession)
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!xs().running() && xs().state() != vr::xr::State::Lost && std::chrono::steady_clock::now() < deadline) {
        xs().poll_events();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!xs().running()) {
        LOG("[vr smoke] FAIL: the session did not start within 15 s (state %s): is the headset awake?",
            vr::xr::state_name(xs().state()));
        return 1;
    }
    int begun = 0, shown = 0, submitted = 0, tracked = 0, hands[2] = {0, 0};
    const auto started = std::chrono::steady_clock::now();
    for (int f = 0; f < frames && xs().running(); ++f) {
        xs().poll_events();
        bool shouldRender = false;
        if (!xs().begin_frame(shouldRender)) continue;
        ++begun;
        vr::xr::Views v;
        const bool located = xs().locate_views(xs().predicted_display_time(), v);
        if (located) ++tracked;
        vr::xr::InputState in;
        if (xs().sync_input(xs().predicted_display_time(), false, in))
            for (int h = 0; h < 2; ++h) hands[h] += in.hand[h].active;
        if (f == frames / 3) for (int h = 0; h < 2; ++h) xs().haptic(h, 0.3f, 0.08f);
        bool drawn = false;
        if (shouldRender && located) {
            ++shown;
            VkImage img[2] = {xs().acquire(0), VK_NULL_HANDLE};
            if (img[0]) img[1] = xs().acquire(1);
            if (img[0] && img[1]) {
                auto cmd = command_buffer();
                for (int eye = 0; eye < 2; ++eye) {
                    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    b.image = img[eye];
                    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    record::vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
                    // a dim colour per eye that brightens over the test: the headset shows the frames arrive
                    const float t = float(f) / float(frames);
                    VkClearColorValue c{};
                    c.float32[0] = eye ? 0.05f : 0.1f + 0.3f * t;
                    c.float32[1] = 0.1f;
                    c.float32[2] = eye ? 0.1f + 0.3f * t : 0.05f;
                    c.float32[3] = 1;
                    record::vkCmdClearColorImage(cmd, img[eye], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &b.subresourceRange);
                    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                    record::vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
                }
                flush();
                drawn = true;
            }
            std::lock_guard<std::mutex> queueLock(graphics_queue_mutex());
            if (img[0]) xs().release(0);
            if (img[1]) xs().release(1);
        }
        float eyeX[2];
        const vr::xr::Views r = located ? vr::as_rendered(v, eyeX) : vr::xr::Views{};
        {
            std::lock_guard<std::mutex> queueLock(graphics_queue_mutex());
            xs().end_frame(r, drawn);
        }
        g_frame = false;
        submitted += drawn;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    LOG("[vr smoke] %d frames begun, %d to show, %d submitted with both eyes, %d with tracked views, controllers "
        "tracked in %d (left) / %d (right) frames; %.1f frames/s, display period %.2f ms",
        begun, shown, submitted, tracked, hands[0], hands[1], begun / std::max(seconds, 1e-3),
        double(xs().predicted_period()) / 1e6);
    if (!hands[0] || !hands[1]) {
        const std::string left = xs().interaction_profile(0), right = xs().interaction_profile(1);
        LOG("[vr smoke] controller profiles: left %s, right %s%s", left.empty() ? "none" : left.c_str(),
            right.empty() ? "none" : right.c_str(),
            left.empty() && right.empty() ? " (controllers off or asleep, or no binding for them)" : "");
    }
    if (begun >= frames / 2 && !shown) {
        // the frame loop ran, but the runtime never showed this application (headset not worn, asleep, or
        // another application / the dashboard in front): nothing more can be checked
        LOG("[vr smoke] PASS (frame loop only): the runtime did not show the frames (session state %d: put the "
            "headset on and close the dashboard to check the pictures and controllers)", int(xs().xr_state()));
        return 0;
    }
    if (submitted < frames / 2) {
        LOG("[vr smoke] FAIL: fewer than half the frames reached the headset");
        return 1;
    }
    LOG("[vr smoke] PASS: OpenXR frame loop, swapchains and projection layer on the headset");
    return 0;
}
#endif
}  // namespace gfxvk::vrvk

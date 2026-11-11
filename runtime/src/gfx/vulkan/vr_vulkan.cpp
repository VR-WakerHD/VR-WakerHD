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
#include "vr_sgsr2.h"
#include "vr_fsr1.h"
#include "motion_pass.h"
#include "vr_framesynth.h"
#include "fov_diag.h"
#include "vr/vr_framesynth.h"
#include "ngx_vk.h"
#include "vr/vr_upscale.h"
#include <unordered_map>
namespace gx2 { void lock_vsync_to_display(int64_t period); void delay_vsync_grid(int64_t ns); }  // (gx2_core.cpp)
namespace vr::game { bool menu_open(); bool name_entry_open(); }  // (vr_game.cpp: the VR menu, the new save's name)
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
    vr::xr::Views views;
    vr::xr::Quad panel, wheel, uiQuad;  // (uiQuad: the game's UI layer, docs/VR_EYE_PROJECTION.md; shown again with the eyes)
    vr::xr::SubQuad wristQuads[vr::kWristPanels], laserQuads[2];
} g_shown;
// the game's own frames (frame_end, not headset_frame): when the last one went out, and the recent interval between
// them (ns, a running average), so that headset_frame waits for a real stall rather than the next frame
std::chrono::steady_clock::time_point g_lastGameFrame{};
double g_gameFrameInterval = 0;
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
    const auto released = std::chrono::steady_clock::now();
    vr::xr::Quad panel = j.panelQuad, wheel = j.wheelQuad, ui = j.uiQuad;
    xs().end_frame(j.views, j.drawn, &panel, &wheel, j.wrist ? j.wristQuads : nullptr, j.wrist ? vr::kWristPanels : 0,
                   j.laser ? j.laserQuads : nullptr, j.laser ? 2 : 0, j.ui ? &ui : nullptr);
    const auto done = std::chrono::steady_clock::now();
    queueMs = ms(queueStart, submitStart);
    releaseMs = ms(submitStart, released);
    submitMs = ms(submitStart, done);
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
        vr_sgsr2::forget();
        vr_fsr1::forget();
        vr_framesynth::forget_views();
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
        const uint32_t w = std::max(64u, uint32_t(std::lround(float(xs().recommended_width()) * headsetRes)));
        const uint32_t h = std::max(64u, uint32_t(std::lround(float(xs().recommended_height()) * headsetRes)));
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
        const bool synthesis = xs().has_synthesis_swapchains();
        vr_framesynth::set_active(synthesis || xs().depth_submission());
        // (only while frames carry the motion vectors: menus and loading screens have none, and keep vr.frameRate)
        interp::set_vr_frame_mode(testRate >= 0 ? testRate
                                  : synthesis && vr_framesynth::carrying() ? int(vr::FrameRate::Half)
                                                                           : int(c.frameRate));  // (from the next logic step)
        if (g_started) request_refresh_rate(c);       // (vr.refreshRate changed in the game's settings)
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
    std::lock_guard<std::mutex> queueLock(graphics_queue_mutex());
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
                      double(c.upscaleSharpness));
        if (last != line) {
            last = line;
            LOG("[vr] upscaling: %s", line);
            vr::upscale::set_status(line);
        }
        xs().set_super_resolution(choice.superResolution);
        // the motion pass only while something uses it (motion_pass::set_needed): SGSR2 in use, the runtime's frame
        // synthesis (its swapchains: motion vectors each frame), or DLSS
        if (motion_pass::on())
            motion_pass::set_needed(choice.sgsr2 || xs().has_synthesis_swapchains() || motion_pass::dlss_runs());
        struct Upscale { bool sgsr1, sgsr2, fsr1, superResolution; float sharpness, fsrSharpness; };
        return Upscale{choice.sgsr1, choice.sgsr2, choice.fsr1, choice.superResolution, c.upscaleSharpness,
                       c.fsrSharpness};
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
    if (g_panelShown && g_panelMenu) {
        VkImage panel = ensure_swapchain(vr::xr::Session::kPanel, kMenuPanelWidth, kMenuPanelHeight,
                                        [] { return xs().ensure_panel(kMenuPanelWidth, kMenuPanelHeight); }) ? xs().acquire(vr::xr::Session::kPanel) : VK_NULL_HANDLE;
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
            if (view && compose_overlay_into(panel, view, layout, {sc.width, sc.height}, VkFormat(sc.format))) g_panelRecorded = true;
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
        VkImage panel = ensure_swapchain(vr::xr::Session::kPanel, w, h, [&] { return xs().ensure_panel(w, h); }) ? xs().acquire(vr::xr::Session::kPanel) : VK_NULL_HANDLE;
        if (panel) {
            const auto& sc = xs().swapchain(vr::xr::Session::kPanel);
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
            blit.dstOffsets[1] = {int32_t(sc.width), int32_t(sc.height), 1};
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
#ifdef __ANDROID__
    // (a PC keeps its desktop window: a mirror for the room, captures and streams)
    return stereo::frame.mode == stereo::XR && vr::active();
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
    end_thread::Job job;
    job.views = v;
    job.drawn = views;
    job.eyes = g_recorded;
    job.panel = g_panelRecorded, job.wheel = g_wheelRecorded, job.wrist = g_wristRecorded, job.laser = g_laserRecorded;
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
    g_shown.panel = panel;
    g_shown.wheel = wheel;
    g_shown.wrist = g_wristRecorded;
    g_shown.laser = g_laserRecorded;
    g_shown.ui = job.ui;
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
        const XrDuration period = xs().predicted_period();
        if (t.lastDisplay && period > 0) t.display.push_back(float(double(shown - t.lastDisplay) / double(period)));
        if (views) note_display(shown, t.lastDisplay);
        t.lastDisplay = shown;
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
int64_t headset_frame_due() {
    if (!g_started || g_frame || !g_shown.valid || !xs().running()) return 0;
    const XrDuration period = xs().predicted_period();
    if (period <= 0 || g_times.lastEnd.time_since_epoch().count() == 0) return 0;
    const int64_t wait = std::max<int64_t>(3 * int64_t(period), int64_t(2.0 * g_gameFrameInterval));
    const auto due = g_times.lastEnd + std::chrono::nanoseconds(wait);
    return std::chrono::duration_cast<std::chrono::nanoseconds>(due.time_since_epoch()).count();
}
void headset_frame() {
    const int64_t due = headset_frame_due();
    const auto now = std::chrono::steady_clock::now();
    if (!due || std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count() < due) return;
    end_thread::wait();  // (the last frame's end, long done: 3 display periods have passed)
    std::unique_lock<std::mutex> queueLock(graphics_queue_mutex(), std::try_to_lock);
    if (!queueLock) return;
    xs().poll_events();
    if (!xs().running()) return;
    bool shouldRender = false;
    if (!xs().begin_frame(shouldRender)) return;
    // (the game's UI layer too: its last released image, as the eyes'. Left out, the HUD, text and menus of the per-eye
    // projection's UI layer blinked off for every frame shown again: loads, shader waits)
    vr::xr::Quad panel = g_shown.panel, wheel = g_shown.wheel, ui = g_shown.uiQuad;
    xs().end_frame(g_shown.views, g_shown.drawn, &panel, &wheel, g_shown.wrist ? g_shown.wristQuads : nullptr,
                   g_shown.wrist ? vr::kWristPanels : 0, g_shown.laser ? g_shown.laserQuads : nullptr, g_shown.laser ? 2 : 0,
                   g_shown.ui ? &ui : nullptr);
    ++g_stats.keptAlive;
    g_times.lastEnd = std::chrono::steady_clock::now();
    // (standalone: the CPU at Boost while the game is stalled, Sustained High again 2 s after the last stall: frame_end)
    g_boostUntil = g_times.lastEnd + std::chrono::seconds(2);
    xs().cpu_boost(true);
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

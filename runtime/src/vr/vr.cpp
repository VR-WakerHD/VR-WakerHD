// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// VR mode state: see vr.h.
#include "vr.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <mutex>

#include "overlay/hostui.h"
#include "runtime.h"
#include "vr_arm.h"
#include "vr_bindings.h"

namespace vr {
namespace {
std::mutex g_mutex;  // config, status, ring, projection
Config g_config;
Bindings g_bindings;
std::string g_status;
std::atomic<int> g_launch{-1};
std::atomic<bool> g_active{false};
EyeOffsets g_eyes;
Fov g_fov;
bool g_fovOn = false;

struct Slot {
    uint32_t id = 0;
    xr::Views views;
    double gameTime = -1;
};
std::array<Slot, kViewRing> g_ring;
uint32_t g_next = 1;
std::atomic<uint32_t> g_frameViews{0}, g_renderViews{0};
}  // namespace

void set_launch_request(int request) { g_launch = request; }

bool wanted() {
    if (const char* e = std::getenv("WWHD_VR")) return !std::strcmp(e, "1");
    const int l = g_launch.load();
    if (l >= 0) return l == 1;
    return config().enabled;
}

Config config() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_config;
}

namespace {
std::atomic<float> g_linkArm{kLinkArmUnits};  // shoulder to wrist in game units, as measured in the HD model
}
float world_scale() {
    const Config c = config();
    if (!c.matchLinkBody || !(c.realArmLength > 0)) return c.unitsPerMetre;
    return world_scale_for_arms(g_linkArm.load(), c.realArmLength);
}
void set_link_arm_length(float units) {
    if (std::isfinite(units) && units > 5.0f && units < 150.0f) g_linkArm.store(units);
}

void set_config(const Config& c) {
    Config v = c;
    validate(v);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_config = v;
    }
    for (const auto& [k, value] : to_kv(v)) hostui::set(k.c_str(), value);
}

void load_settings() {
    std::vector<std::pair<std::string, std::string>> saved;
    for (const auto& k : keys()) {
        std::string v;
        if (hostui::get(k.c_str(), v)) saved.emplace_back(k, v);
    }
    const Config c = from_saved(saved);
    Bindings b;
    for (const auto& [k, unused] : Bindings{}.to_kv()) {
        std::string v;
        if (hostui::get(k.c_str(), v) && !b.from_kv(k, v)) LOG("[vr] setting %s: unknown button \"%s\" (default kept)", k.c_str(), v.c_str());
    }
    {
        std::string f;
        const int format = hostui::get("vr.format", f) ? std::atoi(f.c_str()) : 1;
        const uint32_t offGrip = b.button[int(Action::OffGrip)], mainTrigger = b.button[int(Action::MainTrigger)];
        migrate_bindings(b, format);
        if (offGrip != b.button[int(Action::OffGrip)])
            LOG("[vr] settings format %d: the shield hand's grip no longer presses ZR (the right stick down crouches)", format);
        if (mainTrigger != b.button[int(Action::MainTrigger)])
            LOG("[vr] settings format %d: the sword hand's trigger no longer presses A (it opens the item wheel)", format);
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_config = c;
    g_bindings = b;
}

Bindings bindings() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_bindings;
}
void set_bindings(const Bindings& b) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_bindings = b;
    }
    for (const auto& [k, v] : b.to_kv()) hostui::set(k.c_str(), v);
}

std::string status() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_status;
}
void set_status(const std::string& s) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_status == s) return;
        g_status = s;
    }
    if (!s.empty()) LOG("[vr] %s", s.c_str());
}

bool active() { return g_active.load(std::memory_order_acquire); }
static std::atomic<float> g_layoutScale{1.0f};
// WWHD_VR_TEST_LAYOUT=scale:offset (tests, without a headset): every layout drawn at that scale and moved by that
// offset, as conversations are in a VR session (with WWHD_SETTINGS stereo, an eye-shaped picture)
static bool test_layout(float& scale, float& offset) {
    static float s = 0, y = 0;
    static const bool on = [] {
        const char* e = std::getenv("WWHD_VR_TEST_LAYOUT");
        return e && std::sscanf(e, "%f:%f", &s, &y) == 2 && s > 0.1f && s <= 1.0f;
    }();
    scale = s;
    offset = y;
    return on;
}
float layout_scale() {
    float s, y;
    if (test_layout(s, y)) return s;
    return active() ? g_layoutScale.load() : 1.0f;
}
void set_layout_scale(float s) { g_layoutScale = std::isfinite(s) && s > 0.1f && s <= 3.0f ? s : 1.0f; }
static std::atomic<float> g_layoutY{0.0f};
float layout_offset_y() {
    float s, y;
    if (test_layout(s, y)) return y;
    return active() ? g_layoutY.load() : 0.0f;
}
namespace {
std::atomic<float> g_eyepieceW{0}, g_eyepieceH{0};
}
void set_eyepiece(float width, float height) {
    g_eyepieceW = std::isfinite(width) ? std::clamp(width, 0.0f, 1.0f) : 0.0f;
    g_eyepieceH = std::isfinite(height) ? std::clamp(height, 0.0f, 1.0f) : 0.0f;
}
bool eyepiece(float& width, float& height) {
    width = g_eyepieceW.load();
    height = g_eyepieceH.load();
    return active() && width > 0.0f && height > 0.0f && (width < 1.0f || height < 1.0f);
}
bool layouts_shrunk() {
    float w, h;
    return layout_scale() < 1.0f && !eyepiece(w, h);
}
// (up to 4 half views either way: a conversation's text held in the world goes out of view as you look away)
void set_layout_offset_y(float y) { g_layoutY = std::isfinite(y) ? std::fmax(-4.0f, std::fmin(4.0f, y)) : 0.0f; }
static std::atomic<float> g_layoutX{0.0f};
float layout_offset_x() { return active() ? g_layoutX.load() : 0.0f; }
void set_layout_offset_x(float x) { g_layoutX = std::isfinite(x) ? std::fmax(-4.0f, std::fmin(4.0f, x)) : 0.0f; }
namespace {
std::mutex g_talkDirMutex;
bool g_talkDirOn = false;
Vec3 g_talkDir{0, 0, -1};
}  // namespace
void set_talk_text_dir(bool on, Vec3 dir) {
    std::lock_guard<std::mutex> lock(g_talkDirMutex);
    g_talkDirOn = on && std::isfinite(dir.x) && std::isfinite(dir.y) && std::isfinite(dir.z) && length(dir) > 1e-4f;
    if (g_talkDirOn) g_talkDir = normalize(dir);
}
bool talk_text_dir(Vec3& dir) {
    std::lock_guard<std::mutex> lock(g_talkDirMutex);
    dir = g_talkDir;
    return g_talkDirOn && active();
}
void set_active(bool on) { g_active.store(on, std::memory_order_release); }
namespace {
std::atomic<bool> g_appBackground{false}, g_xrUnfocused{false};
// (2026-11-08) the session has been focused since it last started: only then does losing focus hold the game. A Quest
// 3S log: the session stayed visible, never focused, from the start; the game was held at its first frame for the whole
// run, a black screen (before the pause, such a start played). The app sent to the background holds it always
std::atomic<bool> g_xrFocusedOnce{false};
void note_away(const char* what, bool on) {
    LOG("[vr] headset %s: %s (%s)", on ? "away" : "back", what,
        on ? "the game held and the record worker stopped until it is back" : "the game runs again");
}
}  // namespace
void set_app_background(bool on) {
    if (g_appBackground.exchange(on) != on) note_away(on ? "the app went to the background" : "the app is in the foreground", on);
}
void set_xr_unfocused(bool on) {
    if (!on) g_xrFocusedOnce.store(true, std::memory_order_relaxed);
    if (g_xrUnfocused.exchange(on) == on) return;
    if (on && !g_xrFocusedOnce.load(std::memory_order_relaxed))
        LOG("[vr] the session is not focused yet: the game runs (held only once it has had focus)");
    else
        note_away(on ? "the session lost focus" : "the session has focus", on);
}
void set_xr_session_idle() { g_xrFocusedOnce.store(false, std::memory_order_relaxed); }
bool headset_away() {
#if defined(__ANDROID__) || defined(WWHD_STEAM_FRAME_NATIVE) || (defined(__linux__) && defined(__aarch64__))
    // (Steam Frame - Native, 2026-11-08: the session's focus only; the app's background is Android's)
    return g_appBackground.load(std::memory_order_relaxed) ||
           (g_xrUnfocused.load(std::memory_order_relaxed) && g_xrFocusedOnce.load(std::memory_order_relaxed));
#else
    return false;
#endif
}

xr::Session& session() {
    static xr::Session s;
    return s;
}

uint32_t store_views(const xr::Views& v, double gameTime) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint32_t id = g_next++;
    if (!g_next) g_next = 1;
    auto& slot = g_ring[id % kViewRing];
    slot.id = id;
    slot.views = v;
    slot.gameTime = gameTime;
    return id;
}
bool views(uint32_t id, xr::Views& out, double* gameTime) {
    if (!id) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto& slot = g_ring[id % kViewRing];
    if (slot.id != id) return false;
    out = slot.views;
    if (gameTime) *gameTime = slot.gameTime;
    return true;
}
void set_frame_views(uint32_t id) { g_frameViews = id; }
uint32_t take_frame_views() { return g_frameViews.exchange(0); }
void set_render_views(uint32_t id) { g_renderViews = id; }
uint32_t render_views() { return g_renderViews.load(); }

float fov_margin() {
    static const float m = [] {
        if (const char* e = std::getenv("WWHD_VR_FOV_MARGIN")) return std::clamp(float(std::atof(e)), 0.0f, 20.0f);
        // standalone headsets (Quest; Steam Frame - Native, 2026-11-03: black edges in a turn at 36 fps): the runtime's
        // reprojection between frames reaches past the eyes' fields of view
#if defined(__ANDROID__) || defined(WWHD_STEAM_FRAME_NATIVE) || (defined(__linux__) && defined(__aarch64__))
        return 5.0f;
#else
        return 0.0f;
#endif
    }() * 3.14159265f / 180.0f;
    return m;
}

bool own_eye_projection() {
    // (docs/VR_EYE_PROJECTION.md) each eye drawn with its own field of view, on by default (PS VR2, 2026-11-03: shadows,
    // reflections, menus and the game's UI layer checked); WWHD_VR_EYE_PROJECTION=shared: the symmetric field both eyes
    // share, each eye cropped at submission
    static const bool on = [] {
        const char* e = std::getenv("WWHD_VR_EYE_PROJECTION");
        const bool v = !e || !*e || !(!std::strcmp(e, "shared") || !std::strcmp(e, "0"));
        LOG("[vr] eye projection: %s", v ? "each eye's own field of view (WWHD_VR_EYE_PROJECTION=shared turns it off)"
                                         : "the symmetric field both eyes share, each eye submitted as its own part");
        return v;
    }();
    return on;
}
Fov drawn_fov(const xr::Views& v) { return widen(symmetric_union(v.eye[0].fov, v.eye[1].fov), fov_margin()); }

xr::Views as_rendered(const xr::Views& v, float eyeX[2]) {
    xr::Views r = v;
    // (the margin: the submitted crop, openxr_session.cpp, keeps it beyond each eye's own field of view)
    const Fov u = drawn_fov(v);
    const bool ownProjection = own_eye_projection();
    const Quat inv = conjugate(v.head.orientation);
    for (int e = 0; e < 2; ++e) {
        const Vec3 local = rotate(inv, v.eye[e].pose.position - v.head.position);
        eyeX[e] = local.x;
        r.eye[e].pose = eye_pose(v.head, local.x);
        r.eye[e].fov = u;
        // the part submitted: the eye's own field of view and the margin (openxr_session.cpp fov_crop)
        const bool own = v.eye[e].display.left || v.eye[e].display.right || v.eye[e].display.up || v.eye[e].display.down;
        if (own) r.eye[e].display = widen(v.eye[e].display, fov_margin());
        // (each eye drawn with its own field of view: its whole image is that field, nothing to crop)
        if (ownProjection && own) r.eye[e].fov = r.eye[e].display;
    }
    return r;
}

EyeOffsets eye_offsets() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_eyes;
}
void set_eye_offsets(const EyeOffsets& e) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_eyes = e;
}
bool projection(Fov& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    out = g_fov;
    return g_fovOn;
}
void set_projection(const Fov& f, bool on) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_fov = f;
    g_fovOn = on;
}
namespace {
std::atomic<float> g_eyeImageAspect{0};
}  // namespace
float eye_image_aspect() { return g_eyeImageAspect.load(std::memory_order_relaxed); }
void set_eye_image_aspect(float a) { g_eyeImageAspect.store(a, std::memory_order_relaxed); }
namespace {
std::atomic<uint64_t> g_pictureSize{0};  // width << 32 | height
}  // namespace
void set_picture_size(uint32_t width, uint32_t height) {
    g_pictureSize.store(uint64_t(width) << 32 | height, std::memory_order_relaxed);
}
bool picture_size(uint32_t& width, uint32_t& height) {
    const uint64_t v = g_pictureSize.load(std::memory_order_relaxed);
    width = uint32_t(v >> 32), height = uint32_t(v);
    return width && height;
}
namespace {
std::mutex g_eyeFieldMutex;
Fov g_eyeFields[2];
bool g_eyeFieldsOn = false;
}  // namespace
bool eye_field(int eye, Fov& out) {
    std::lock_guard lock(g_eyeFieldMutex);
    if (!g_eyeFieldsOn || eye < 0 || eye > 1) return false;
    out = g_eyeFields[eye];
    return true;
}
void set_eye_fields(const Fov& left, const Fov& right, bool on) {
    std::lock_guard lock(g_eyeFieldMutex);
    g_eyeFields[0] = left, g_eyeFields[1] = right, g_eyeFieldsOn = on;
}

namespace {
std::atomic<uint64_t> g_gameWork[int(GameWork::Count)];
uint64_t now_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
}  // namespace
void add_game_work(GameWork part, uint64_t ns) { g_gameWork[int(part)].fetch_add(ns, std::memory_order_relaxed); }
void take_game_work(uint64_t (&ns)[int(GameWork::Count)]) {
    for (int i = 0; i < int(GameWork::Count); ++i) ns[i] = g_gameWork[i].exchange(0, std::memory_order_relaxed);
}
GameWorkTimer::GameWorkTimer(GameWork part) : part_(part), start_(now_ns()) {}
GameWorkTimer::~GameWorkTimer() { add_game_work(part_, now_ns() - start_); }

}  // namespace vr

#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "stereo_projection.h"

// Output packing is independent of the aspect ratio of one eye.
namespace stereo {
// Count: the modes the 3D output setting offers. XR is the VR headset's (vr/vr.h): both eyes drawn at full
// size into their own images, parallel cameras (no convergence) offset by the headset's measured eyes;
// the window shows the left eye. Only the VR session selects it (set_xr), never the saved setting.
enum Mode { Off, HalfSBS, FullSBS, TAB, Count, XR = Count };
inline constexpr float eyeDistanceAtUnitDepth = 3.2f;
inline std::atomic<int> requestedMode{Off};
inline std::atomic<float> requestedDepth{1.0f}, requestedConvergence{600.0f};
inline std::atomic<bool> requestedSwap{false};
inline std::atomic<bool> requestedXR{false};
// the OpenXR runtime made the Vulkan instance (vr_vulkan.cpp), until VR stops: the windows' swapchains are made for
// VR's present mode from the start, so the session's start does not remake them (each remake is a new swapchain
// for capture hooks such as OBS's to wrap; one crashed the NVIDIA driver at that point, a Quest 2 on VDXR)
inline std::atomic<bool> startingXR{false};
inline std::atomic<float> requestedEyeOffset[2]{{0.0f}, {0.0f}};
inline void set_mode(int v) { requestedMode = v >= Off && v < Count ? v : Off; }
// --no-stereo (the launchers' "Play Flat"): this start is the plain desktop picture, whatever 3D output the
// settings saved; the setting itself is kept (F1 can still turn 3D on, and saves it then)
inline std::atomic<bool> launchMono{false};
inline void set_depth(float v) { requestedDepth = std::isfinite(v) ? std::clamp(v, 0.0f, 5.0f) : 1.0f; }
inline void set_convergence(float v) { requestedConvergence = std::isfinite(v) ? std::clamp(v, 10.0f, 10000.0f) : 600.0f; }
// XR: eye offsets in view-space game units (left negative); takes effect at the next frame boundary
inline void set_xr(bool on, float left = 0, float right = 0) {
    requestedEyeOffset[0] = std::isfinite(left) ? left : 0.0f;
    requestedEyeOffset[1] = std::isfinite(right) ? right : 0.0f;
    requestedXR = on;
}
// XR (docs/VR_EYE_PROJECTION.md): each eye's own field of view within the symmetric one the camera draws (sx, ox, sy,
// oy; stereo_projection.h EyeFrustum); the default draws both eyes with the symmetric field. Next frame boundary
inline std::atomic<float> requestedFrustum[2][4]{{1, 0, 1, 0}, {1, 0, 1, 0}};
inline void set_eye_frustums(const EyeFrustum& left, const EyeFrustum& right) {
    const EyeFrustum* f[2] = {&left, &right};
    for (int e = 0; e < 2; ++e) {
        requestedFrustum[e][0] = f[e]->sx, requestedFrustum[e][1] = f[e]->ox;
        requestedFrustum[e][2] = f[e]->sy, requestedFrustum[e][3] = f[e]->oy;
    }
}
inline bool requested_any() { return requestedXR.load() || requestedMode.load() != Off; }
struct Frame {
    int mode = Off;
    float depth = 1, convergence = 600;
    bool swap = false;
    float eyeOffset[2] = {0, 0};  // XR only
    EyeFrustum frustum[2];        // XR: each eye's own field of view (identity: the symmetric field)
};
inline Frame frame; // renderer-thread only; settings take effect at a frame boundary
// The command stream brackets nw::lyt Pane::DrawSelf. Projection values alone
// cannot distinguish a layout from a 3D camera with the same lens parameters.
inline unsigned layoutDrawDepth = 0;
// The game's captured-photo pane samples a single canonical image in both eyes.
inline bool layoutPhotograph = false;
inline void layout_draw(bool begin) {
    if (begin) ++layoutDrawDepth;
    else if (layoutDrawDepth) --layoutDrawDepth;
}
// (docs/VR_SCENE_THREADS.md, stage S6) the second drawing thread's own copy, taken when its span starts: the scene's
// thread may still be applying earlier state to the shared one while it draws
struct LayoutState { unsigned drawDepth = 0; bool photograph = false; };
inline thread_local const LayoutState* t_layoutOverride = nullptr;
inline unsigned layout_draw_depth() { return t_layoutOverride ? t_layoutOverride->drawDepth : layoutDrawDepth; }
inline bool layout_photograph() { return t_layoutOverride ? t_layoutOverride->photograph : layoutPhotograph; }
inline uint32_t layoutProjectionStart = 0;
inline std::array<uint32_t,16> layoutProjection{};
inline void tag_layout(uint32_t start, const uint32_t* words) {
    layoutProjectionStart=start;
    std::memcpy(layoutProjection.data(),words,64);
}
// (docs/VR_EYE_PROJECTION.md, "The game's UI as its own layer") the eyes drawn with their own frustums in XR: the game's
// layouts go into the UI layer's image, which has the symmetric field's shape (aspect::game()), not an eye's
inline bool ui_layer_on() {
    return frame.mode == XR && !(frame.frustum[0].identity() && frame.frustum[1].identity());
}
// the factors that place the 16:9 layout space in a picture of shape `shared` undistorted: wider pictures widen x,
// narrower ones lengthen y (the UI image's; an eye picture's are surfaces.cpp latch_aspect's, narrower still)
inline void ui_layer_factors(float shared, float& kx, float& ky) {
    const float base = 16.0f / 9.0f;
    if (!(shared > 0.0f)) shared = base;
    kx = shared >= base ? shared / base : 1.0f;
    ky = shared >= base ? 1.0f : base / shared;
}
inline bool is_layout(const uint32_t* regs) {
    return layoutProjectionStart && !std::memcmp(regs+layoutProjectionStart,layoutProjection.data(),64);
}
inline void latch() {
    if (requestedXR.load()) {
        // parallel eyes: an infinite convergence leaves only the eye's translation (project_eye, clip_shift)
        frame = {XR, 1.0f, INFINITY, false, {requestedEyeOffset[0].load(), requestedEyeOffset[1].load()}};
        for (int e = 0; e < 2; ++e)
            frame.frustum[e] = {requestedFrustum[e][0].load(), requestedFrustum[e][1].load(), requestedFrustum[e][2].load(),
                                requestedFrustum[e][3].load()};
        return;
    }
    frame = {requestedMode.load(), requestedDepth.load(), requestedConvergence.load(), requestedSwap.load()};
    // (test aid, docs/VR_EYE_PROJECTION.md) WWHD_TEST_EYE_FRUSTUM=sx,ox,sy,oy: both desktop-stereo eyes drawn with that
    // frustum, so a picture can be compared with the same part of the symmetric one (unattended desktop runs)
    static const EyeFrustum test = [] {
        EyeFrustum f;
        if (const char* e = std::getenv("WWHD_TEST_EYE_FRUSTUM"))
            if (std::sscanf(e, "%f,%f,%f,%f", &f.sx, &f.ox, &f.sy, &f.oy) != 4 || !(f.sx > 0) || !(f.sy > 0)) f = {};
        return f;
    }();
    if (frame.mode != Off) frame.frustum[0] = frame.frustum[1] = test;
}
// the view-space offset of an eye (0 left, 1 right) for this frame's 3D camera
inline float eye_offset(int eye) {
    if (frame.mode == XR) return frame.eyeOffset[eye ? 1 : 0];
    return (eye ? 1.0f : -1.0f) * frame.depth * eyeDistanceAtUnitDepth;
}
// the frustum of the eye a projection is shifted for (the offset's sign: left negative; 0, no eye: none)
inline EyeFrustum eye_frustum(float eyeOffset) {
    if (eyeOffset == 0) return {};
    return frame.frustum[eyeOffset > 0 ? 1 : 0];
}
// (docs/VR_EYE_PROJECTION.md) this thread's draw is a layout (HUD, menus, text) drawn into an eye's picture in XR: set
// per draw. A layout is not shifted for the eye (it is flat, at no depth), but each eye's own picture covers its own
// field, so it takes its eye's frustum: the same place ahead of both eyes (else it showed twice)
inline thread_local bool t_layoutInEye = false;
// the frustum for a projection of an eye's draw: by its eye offset. A layout's (no offset) is not patched: with each
// eye's own projection the game's UI goes to its own image, shown as a quad layer (docs/VR_EYE_PROJECTION.md)
inline EyeFrustum draw_frustum(float eyeOffset, int eye) {
    (void)eye;
    return eye_frustum(eyeOffset);
}
// the frustum for recorded rows replayed for an eye: a layout's by the eye, else by the offset
inline EyeFrustum rows_frustum(const EyeRows& rows, float eyeOffset, int eye) {
    if (rows.layout && eyeOffset == 0) return frame.frustum[eye ? 1 : 0];
    return eye_frustum(eyeOffset);
}
// the output packs both eyes into the window's picture (side by side or top and bottom)
inline bool packed() { return frame.mode != Off && frame.mode != XR; }
struct Rect { float x, y, w, h; };
inline Rect pack(Rect r, float width, float height, int mode, int eye) {
    if (mode == TAB) return {r.x, (r.y + eye * height) * .5f, r.w, r.h * .5f};
    return {(r.x + eye * width) * .5f, r.y, r.w * .5f, r.h};
}
// Parallel cameras with an asymmetric frustum. clipW is camera-space distance;
// no depth buffer is read and newly exposed geometry is rasterized for each eye.
inline float clip_shift(float clipW, float eyeOffset, float convergence) {
    return eyeOffset * (clipW / convergence - 1.0f);
}
}

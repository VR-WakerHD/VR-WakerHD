// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// (diagnostics, docs/VR_EYE_PROJECTION.md) the projection each eye's draws actually upload: found in the uniform bytes
// after the eye shift (an eye's projection has an off-axis x and y and a translation, which stereo::camera_projection
// does not take), kept per eye, logged every 10 s by vr_vulkan.cpp beside the field of view each eye was submitted with.
// WWHD_VR_FOV_DIAG=1 turns it on; off, it costs one branch where the eye uniforms are made.
#pragma once
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

namespace gfxvk::fov_diag {

inline const bool kOn = [] {
    const char* e = std::getenv("WWHD_VR_FOV_DIAG");
    return e && e[0] == '1';
}();

// the last projection seen for an eye: its x and y rows' scale and offset (p0, p2, p5, p6)
struct Seen {
    std::atomic<float> p0{0}, p2{0}, p5{0}, p6{0};
    std::atomic<unsigned> count{0};
};
inline Seen g_seen[2];

// a perspective projection after an eye shift: GX2's row-major matrix with w = -z, any x/y offset and translation
inline bool shifted_projection(const float* p) {
    if (p[12] != 0 || p[13] != 0 || p[14] != -1 || p[15] != 0) return false;
    if (!(p[0] > 0 && p[5] > 0 && p[1] == 0 && p[4] == 0 && p[8] == 0 && p[9] == 0 && p[10] <= -1 && p[11] < 0))
        return false;
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(p[i])) return false;
    return true;
}
// any thread: the eye uniform bytes of a draw (eye: its offset's sign, left negative; 0 is no eye)
// (sampled: one draw in 64 per thread and eye, so the scan and the shared stores stay off most draws; a count per eye,
// since a draw notes eye 0 and then eye 1: one count for both only ever sampled eye 0)
inline bool sample(float eyeOffset) {
    static thread_local unsigned n[2] = {0, 0};
    return (n[eyeOffset > 0 ? 1 : 0]++ & 63) == 0;
}
inline void note(float eyeOffset, const unsigned char* bytes, size_t size) {
    if (!kOn || eyeOffset == 0 || !bytes || !sample(eyeOffset)) return;
    for (size_t at = 0; at + 64 <= size; at += 16) {
        float p[16];
        std::memcpy(p, bytes + at, 64);
        if (!shifted_projection(p)) continue;
        Seen& s = g_seen[eyeOffset > 0 ? 1 : 0];
        s.p0.store(p[0], std::memory_order_relaxed);
        s.p2.store(p[2], std::memory_order_relaxed);
        s.p5.store(p[5], std::memory_order_relaxed);
        s.p6.store(p[6], std::memory_order_relaxed);
        s.count.fetch_add(1, std::memory_order_relaxed);
        return;
    }
}
// any thread: a projection's values as an eye's draw got them (its x row and its y row as they were written)
inline void note_values(float eyeOffset, float p0, float p2, float p5, float p6) {
    if (!kOn || eyeOffset == 0 || !sample(eyeOffset)) return;
    Seen& s = g_seen[eyeOffset > 0 ? 1 : 0];
    s.p0.store(p0, std::memory_order_relaxed);
    s.p2.store(p2, std::memory_order_relaxed);
    s.p5.store(p5, std::memory_order_relaxed);
    s.p6.store(p6, std::memory_order_relaxed);
    s.count.fetch_add(1, std::memory_order_relaxed);
}
// (headset data, docs/VR_EYE_PROJECTION.md) the layouts (HUD, menus, text) drawn into each eye's picture, and how each
// got its eye's frustum: in the uniform packer, a uniform block, a batch's pieces, eye 1's replay; or not at all (and
// which vertex program those use)
struct LayoutStats {
    std::atomic<unsigned> draws{0}, pack{0}, block{0}, batch{0}, replay{0}, none{0};
};
inline LayoutStats g_layout[2];
inline std::mutex g_layoutMutex;
inline std::map<uint32_t, unsigned> g_unpatched[2];
inline thread_local bool t_layoutDraw = false, t_layoutPatched = false;
inline thread_local int t_layoutEye = 0;
inline thread_local uint32_t t_layoutVs = 0;
// each draw (its thread): whether it is a layout into an eye's picture; the previous one's outcome counted
inline void layout_draw_begin(bool layout, int eye, uint32_t vs) {
    if (!kOn) return;
    if (t_layoutDraw && !t_layoutPatched) {
        g_layout[t_layoutEye].none.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> l(g_layoutMutex);
        ++g_unpatched[t_layoutEye][t_layoutVs];
    }
    t_layoutDraw = layout, t_layoutPatched = false, t_layoutEye = eye ? 1 : 0, t_layoutVs = vs;
    if (layout) g_layout[t_layoutEye].draws.fetch_add(1, std::memory_order_relaxed);
}
// a layout's projection got its eye's frustum (path: 0 packer, 1 uniform block, 2 batch)
inline void layout_patched(int path) {
    if (!kOn || !t_layoutDraw || t_layoutPatched) return;
    t_layoutPatched = true;
    LayoutStats& s = g_layout[t_layoutEye];
    (path == 0 ? s.pack : path == 1 ? s.block : s.batch).fetch_add(1, std::memory_order_relaxed);
}
inline void layout_replay(int eye) {
    if (kOn) g_layout[eye ? 1 : 0].replay.fetch_add(1, std::memory_order_relaxed);
}

// the field of view a projection covers, in radians: x_ndc = p0 * tan - p2 at the edges -1 and +1 (y the same)
inline void field(float p0, float p2, float p5, float p6, float& left, float& right, float& up, float& down) {
    left = std::atan((p2 - 1.0f) / p0);
    right = std::atan((p2 + 1.0f) / p0);
    down = std::atan((p6 - 1.0f) / p5);
    up = std::atan((p6 + 1.0f) / p5);
}

}  // namespace gfxvk::fov_diag

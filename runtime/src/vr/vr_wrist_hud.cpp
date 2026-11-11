// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// The VR wrist HUD's picture (vr_wrist_hud.h): drawn on the CPU (vr_canvas.h) only when its contents change.
#include "vr_wrist_hud.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>

#include "vr_canvas.h"

namespace vr {
namespace {
using canvas::Canvas;
using canvas::Colour;

std::mutex g_viewMu;
WristHudView g_view;

constexpr Colour kBacking{0.04f, 0.05f, 0.08f, 0.62f};
constexpr Colour kHeartFull{0.93f, 0.16f, 0.20f, 1.0f}, kHeartEmpty{0.30f, 0.12f, 0.14f, 0.85f};
constexpr Colour kOutline{1.0f, 1.0f, 1.0f, 0.95f}, kGold{0.98f, 0.85f, 0.35f, 1.0f}, kGrey{0.65f, 0.67f, 0.72f, 1.0f};
constexpr Colour kRupee{0.25f, 0.85f, 0.35f, 1.0f};
constexpr Colour kMagicFull{0.30f, 0.86f, 0.22f, 1.0f}, kMagicEmpty{0.10f, 0.22f, 0.10f, 0.85f};
constexpr Colour kFreeRoamRed{0.95f, 0.12f, 0.12f, 1.0f};

// a rounded rectangle in rect `r` (radius `rad`)
void plate(Canvas& c, const WristRect& r, float rad, Colour col) {
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x) {
            const float px = float(x) + 0.5f, py = float(y) + 0.5f;
            const float dx = std::max({float(r.x) + rad - px, 0.0f, px - (float(r.x + r.w) - rad)});
            const float dy = std::max({float(r.y) + rad - py, 0.0f, py - (float(r.y + r.h) - rad)});
            c.blend(x, y, col, std::clamp(rad - std::hypot(dx, dy) + 0.5f, 0.0f, 1.0f));
        }
}
// inside a heart of size s centred at (cx, cy)? Two lobes and a point, as the game's heart icon
float heart_coverage(float px, float py, float cx, float cy, float s) {
    // in the heart's frame: x -1..1, y -1 (top) .. 1 (point)
    const float x = (px - cx) / (s * 0.5f), y = (py - cy) / (s * 0.5f);
    const float lobe = 0.52f;
    const float dl = std::hypot(x + 0.48f, y + 0.38f), dr = std::hypot(x - 0.48f, y + 0.38f);
    const float edgeLobe = lobe - std::min(dl, dr);
    // the lower triangle: from the lobes' outer sides down to the point at (0, 1)
    const float edgeTri = std::min((1.0f - y) * 0.92f - std::fabs(x) * 1.0f, y + 0.4f);
    const float inside = std::max(edgeLobe, edgeTri) * (s * 0.5f);  // pixels inside the edge (approximate)
    return std::clamp(inside + 0.5f, 0.0f, 1.0f);
}
// a heart filled by quarters (0..4: clockwise from the top, as the game's heart fills)
void heart(Canvas& c, float cx, float cy, float s, int quarters) {
    for (int y = int(cy - s * 0.6f); y <= int(cy + s * 0.6f); ++y)
        for (int x = int(cx - s * 0.6f); x <= int(cx + s * 0.6f); ++x) {
            const float px = float(x) + 0.5f, py = float(y) + 0.5f;
            const float outer = heart_coverage(px, py, cx, cy, s * 1.12f);
            if (outer <= 0) continue;
            c.blend(x, y, kOutline, outer);  // a white rim, the game's outline
            const float k = heart_coverage(px, py, cx, cy, s);
            if (k <= 0) continue;
            // its quarter, clockwise from 12 o'clock
            const float a = std::atan2(px - cx, -(py - cy));  // 0 up, + clockwise
            const int q = int(std::floor((a < 0 ? a + 2.0f * 3.14159265f : a) / (3.14159265f * 0.5f))) & 3;
            c.blend(x, y, q < quarters ? kHeartFull : kHeartEmpty, k);
        }
}
// the rupee: an upright gem
void rupee_gem(Canvas& c, float cx, float cy, float h) {
    const float w = h * 0.55f;
    for (int y = int(cy - h * 0.5f) - 1; y <= int(cy + h * 0.5f) + 1; ++y)
        for (int x = int(cx - w * 0.5f) - 1; x <= int(cx + w * 0.5f) + 1; ++x) {
            const float dx = std::fabs(float(x) + 0.5f - cx) / (w * 0.5f), dy = std::fabs(float(y) + 0.5f - cy) / (h * 0.5f);
            // a hexagon: straight sides, pointed top and bottom
            const float inside = std::min(1.0f - dx, (1.0f - dy) * 1.6f - dx * 0.6f);
            c.blend(x, y, kRupee, std::clamp(inside * w * 0.5f + 0.5f, 0.0f, 1.0f));
        }
}

void draw(const WristHudView& v, const std::vector<std::vector<uint8_t>>& icons, Canvas& c) {
    // hearts: one row, as many as the game's maximum, sized to fit
    {
        const WristRect& r = kWristRects[kWristHearts];
        plate(c, r, 18, kBacking);
        const int n = std::max(1, heart_count(v.maxLife));
        const float slot = std::min(float(r.h) - 16.0f, (float(r.w) - 24.0f) / float(n));
        const float size = slot * 0.86f;
        const float x0 = float(r.x) + (float(r.w) - slot * float(n)) * 0.5f + slot * 0.5f, cy = float(r.y) + float(r.h) * 0.5f;
        for (int i = 0; i < heart_count(v.maxLife); ++i) heart(c, x0 + slot * float(i), cy, size, heart_quarters(v.life, i));
    }
    // the item buttons: X, Y, R, each with its letter; an empty button dimmed
    {
        const WristRect& r = kWristRects[kWristItems];
        plate(c, r, 22, kBacking);
        static const char* const letters[3] = {"X", "Y", "R"};
        const float slot = float(r.w) / 3.0f, cy = float(r.y) + float(r.h) * 0.5f, radius = std::min(slot, float(r.h)) * 0.40f;
        for (int b = 0; b < 3; ++b) {
            const float cx = float(r.x) + slot * (float(b) + 0.5f);
            const bool has = v.items[b] != 0xFF && v.icons[b] >= 0 && v.icons[b] < int(icons.size()) && !icons[v.icons[b]].empty();
            c.disc(cx, cy, radius, {0.10f, 0.12f, 0.18f, 0.9f});
            c.ring(cx, cy, radius, 3.0f, has ? kGold : kGrey);
            if (has) c.icon(icons[v.icons[b]], 128, 128, cx, cy, radius * 1.65f, 1.0f);
            const float lx = cx + radius * 0.78f, ly = cy + radius * 0.78f;
            c.disc(lx, ly, 17.0f, {0.05f, 0.05f, 0.07f, 0.95f});
            canvas::text(c, letters[b], lx, ly, 26.0f, has ? kGold : kGrey);
        }
    }
    // the rupees: the gem and the count, left to right
    {
        const WristRect& r = kWristRects[kWristRupees];
        plate(c, r, 18, kBacking);
        const float cy = float(r.y) + float(r.h) * 0.5f;
        if (v.testMode) canvas::text(c, "TEST", float(r.x) + 44.0f, cy, float(r.h) * 0.34f, {1.0f, 0.72f, 0.15f, 1});  // (docs/VR_TEST_ENVIRONMENT.md)
        else rupee_gem(c, float(r.x) + 38.0f, cy, float(r.h) * 0.62f);
        char digits[8];
        std::snprintf(digits, sizeof digits, "%d", std::clamp(v.rupees, 0, 9999));
        const float textX = float(r.x) + 38.0f + (float(r.w) - 38.0f) * 0.5f + 8.0f;
        canvas::text(c, digits, textX, cy, float(r.h) * 0.62f, {1, 1, 1, 1});
    }
    // the magic bar: the game's green meter across its whole box, filled by the magic left, with a white rim
    {
        const WristRect& r = kWristRects[kWristMagic];
        plate(c, r, 18, kBacking);
        const float pad = 8.0f, rim = 4.0f;  // (the bar fills its box: 8 pixels of the box round it)
        const float full = float(r.w) - 2.0f * pad, len = full * magic_bar_length(v.maxMagic);
        if (len > 2.0f * rim) {
            const float h = float(r.h) - 2.0f * pad, x0 = float(r.x) + pad, y0 = float(r.y) + pad;
            const WristRect outer{int(x0), int(y0), int(len), int(h)};
            plate(c, outer, h * 0.5f, kOutline);
            const WristRect inner{int(x0 + rim), int(y0 + rim), int(len - 2.0f * rim), int(h - 2.0f * rim)};
            plate(c, inner, (h - 2.0f * rim) * 0.5f, kMagicEmpty);
            const int filled = int(std::lround(float(inner.w) * magic_bar_fill(v.magic, v.maxMagic)));
            if (filled > 0) plate(c, {inner.x, inner.y, filled, inner.h}, float(inner.h) * 0.5f, kMagicFull);
        }
    }
    // the Free Roam Camera's label: "Free Roam" in red in its own font (Angilla Tattoo, when it is next to the
    // executable: vr_canvas.h Face::Label), a dark shadow under it to read over the bright sea and sky (always in the
    // picture: the quad shows it only while the Free Roam Camera does)
    {
        const WristRect& r = kWristRects[kWristLabel];
        const float cx = float(r.x) + float(r.w) * 0.5f, cy = float(r.y) + float(r.h) * 0.5f;
        float h = float(r.h) * kLabelTextHeight;
        const float room = float(r.w) - 32.0f - 2.0f * kLabelBold, wide = canvas::text_width("Free Roam", h, canvas::Face::Label);
        if (wide > room) h *= room / wide;
        canvas::text(c, "Free Roam", cx + 3.0f, cy + 3.0f, h, {0.0f, 0.0f, 0.0f, 0.75f}, canvas::Face::Label, kLabelBold);
        canvas::text(c, "Free Roam", cx, cy, h, kFreeRoamRed, canvas::Face::Label, kLabelBold);
    }
}

std::vector<uint8_t> g_pixels;
bool g_drawn = false, g_drawnBgra = false;
int g_drawnIcons = -1;
WristHudView g_drawnView;
bool same_contents(const WristHudView& a, const WristHudView& b) {
    return a.life == b.life && a.maxLife == b.maxLife && a.rupees == b.rupees && a.testMode == b.testMode &&
           a.magic == b.magic && a.maxMagic == b.maxMagic &&
           std::equal(a.items, a.items + 3, b.items) && std::equal(a.icons, a.icons + 3, b.icons);
}

}  // namespace

void set_wrist_hud(const WristHudView& v) {
    std::lock_guard<std::mutex> lock(g_viewMu);
    g_view = v;
}
bool wrist_hud_view(WristHudView& out) {
    std::lock_guard<std::mutex> lock(g_viewMu);
    if (std::none_of(g_view.visible, g_view.visible + kWristPanels, [](bool v) { return v; })) return false;
    out = g_view;
    return true;
}

const std::vector<uint8_t>& wrist_hud_pixels(const WristHudView& v, bool bgra) {
    const int iconState = canvas::with_icons(nullptr, nullptr);
    if (g_drawn && bgra == g_drawnBgra && iconState == g_drawnIcons && same_contents(v, g_drawnView)) return g_pixels;
    Canvas c{int(kWristHudWidth), int(kWristHudHeight)};
    struct Ctx { const WristHudView* v; Canvas* c; } ctx{&v, &c};
    canvas::with_icons([](const std::vector<std::vector<uint8_t>>& icons, void* p) {
        auto* x = static_cast<Ctx*>(p);
        draw(*x->v, icons, *x->c);
    }, &ctx);
    canvas::to_pixels(c, bgra, g_pixels);
    g_drawn = true;
    g_drawnBgra = bgra;
    g_drawnIcons = iconState;
    g_drawnView = v;
    return g_pixels;
}

}  // namespace vr

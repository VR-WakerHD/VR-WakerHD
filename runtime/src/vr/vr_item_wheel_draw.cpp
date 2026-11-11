// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// The VR item wheel's picture (vr_item_wheel_draw.h). Drawn on the CPU: the game's own item icons (read from the
// player's 2D pack, item_icons::game_icons, on a worker thread the first time), the ring, the highlight, and the
// captions in a system font (stb_truetype, the copy bundled with Dear ImGui; the overlay's fonts). Nothing here runs per
// frame: the picture is drawn again only when what the wheel shows changes.
#include "vr_item_wheel_draw.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "mods/item_icons.h"
#include "runtime.h"
#include "vr_canvas.h"

namespace vr {
namespace {
using canvas::Canvas;
using canvas::Colour;
using canvas::text;
using canvas::text_width;
using canvas::wrap;

std::mutex g_viewMu;
WheelView g_view;

const char* button_name(int b) { return b == 0 ? "X" : b == 1 ? "Y" : b == 2 ? "R" : "?"; }

void draw(const WheelView& v, const std::vector<std::vector<uint8_t>>& icons, Canvas& c) {
    const float mid = float(c.n) * 0.5f;
    const Colour gold{0.96f, 0.80f, 0.36f, 1.0f}, white{1, 1, 1, 1}, grey{0.72f, 0.75f, 0.80f, 1.0f};
    const int pageCount = int(v.pages.size());
    const int nNow = pageCount ? int(v.pages[std::clamp(v.page, 0, pageCount - 1)].entries.size()) : 0;
    const WheelLayout lay = wheel_layout(float(c.n), nNow);
    c.disc(mid, mid, lay.ring + 2, {0.04f, 0.06f, 0.10f, 0.80f});
    c.ring(mid, mid, lay.ring, 5, {gold.r, gold.g, gold.b, 0.85f});
    if (v.pages.empty()) {
        text(c, "No items", mid, mid, 40, grey);
        return;
    }
    const int pageIndex = std::clamp(v.page, 0, int(v.pages.size()) - 1);
    const WheelPageItems& page = v.pages[pageIndex];
    const int n = int(page.entries.size());
    const int sel = std::clamp(v.index, 0, std::max(0, n - 1));
    // the page's name above the ring, outside it (its place among the pages: in the middle, below)
    char pageLine[64];
    std::snprintf(pageLine, sizeof pageLine, "%s", page_name(page.page));
    text(c, pageLine, mid, (mid - lay.ring) * 0.5f, 34, gold);
    // the ring of items: the first at the top, clockwise, as large as fit, hugging the ring's inside (wheel_layout)
    const float radius = lay.centres;
    const float slot = lay.icon;
    for (int i = 0; i < n; ++i) {
        const WheelEntry& e = page.entries[i];
        float x, y;
        ring_position(i, n, radius, x, y);
        x += mid, y += mid;
        const bool chosen = i == sel;
        const float r = chosen ? slot * kWheelChosenScale : slot;
        c.disc(x, y, r, chosen ? Colour{0.30f, 0.24f, 0.10f, 0.95f} : Colour{0.14f, 0.17f, 0.24f, 0.92f});
        if (chosen) c.ring(x, y, r, 5, gold);
        const int frame = e.item < v.icons.size() ? v.icons[e.item] : -1;
        const float fade = e.empty ? 0.4f : 1.0f;
        if (frame >= 0 && frame < int(icons.size()) && !icons[frame].empty()) {
            c.icon(icons[frame], item_icons::kIconSize, item_icons::kIconSize, x, y, r * 1.7f, fade);
        } else {  // no icon in the game's tables (the sails): the name in the circle
            const float h = std::max(16.0f, r * 0.36f);
            const std::vector<std::string> lines = wrap(item_name(e.item), r * 1.8f, h);
            float ty = y - 0.5f * h * float(lines.size() - 1);
            for (const std::string& l : lines) text(c, l, x, ty, h, {white.r, white.g, white.b, fade}), ty += h;
        }
        if (e.button >= 0) {  // the button it is on now
            const float b = std::clamp(r * 0.28f, 12.0f, 18.0f);
            c.disc(x + r * 0.72f, y + r * 0.72f, b, {0.08f, 0.10f, 0.16f, 0.95f});
            text(c, button_name(e.button), x + r * 0.72f, y + r * 0.72f, b * 1.45f, gold);
        }
    }
    // the chosen item in the middle: its name, its count, what it needs, how to take it, and the page's number, sized to
    // the space inside the icons
    if (n > 0) {
        const WheelEntry& e = page.entries[sel];
        const float inner = std::max(80.0f, radius - slot);  // the free circle inside the icons
        const float k = std::clamp(inner / 190.0f, 0.55f, 1.0f);
        const std::vector<std::string> lines = wrap(item_name(e.item), inner * 1.6f, 38 * k);
        float y = mid - 60 * k - 22.0f * k * float(lines.size() - 1);
        for (const std::string& l : lines) {
            text(c, l, mid, y, 38 * k, white);
            y += 44 * k;
        }
        char line[64] = "";
        if (e.count >= 0) std::snprintf(line, sizeof line, e.empty ? "x %d (empty)" : "x %d", e.count);
        if (*line) text(c, line, mid, y + 2, 30 * k, e.empty ? grey : gold), y += 38 * k;
        if (!e.detail.empty()) text(c, e.detail, mid, y + 2, 24 * k, grey), y += 32 * k;
        y = std::max(y + 10 * k, mid + 40 * k);
        text(c, "Pull Hand Forward to Grab Item", mid, y, 24 * k, grey);
        if (pageCount > 1) {
            std::snprintf(pageLine, sizeof pageLine, "Page %d/%d", pageIndex + 1, pageCount);
            text(c, pageLine, mid, y + 34 * k, 26 * k, gold);
        }
    }
}

std::vector<uint8_t> g_pixels;
uint64_t g_drawnVersion = ~0ull;
bool g_drawnBgra = false;
int g_drawnIcons = -1;

}  // namespace

void set_wheel_view(const WheelView& v) {
    std::lock_guard<std::mutex> lock(g_viewMu);
    g_view = v;
}
bool wheel_view(WheelView& out) {
    std::lock_guard<std::mutex> lock(g_viewMu);
    if (!g_view.visible) return false;
    out = g_view;
    return true;
}

const std::vector<uint8_t>& wheel_pixels(const WheelView& v, bool bgra) {
    const int iconState = canvas::with_icons(nullptr, nullptr);
    if (v.version == g_drawnVersion && bgra == g_drawnBgra && iconState == g_drawnIcons && !g_pixels.empty()) return g_pixels;
    Canvas c{int(kWheelPixels)};
    struct Ctx { const WheelView* v; Canvas* c; } ctx{&v, &c};
    canvas::with_icons([](const std::vector<std::vector<uint8_t>>& icons, void* p) {
        auto* x = static_cast<Ctx*>(p);
        draw(*x->v, icons, *x->c);
    }, &ctx);
    canvas::to_pixels(c, bgra, g_pixels);
    g_drawnVersion = v.version;
    g_drawnBgra = bgra;
    g_drawnIcons = iconState;
    return g_pixels;
}

}  // namespace vr

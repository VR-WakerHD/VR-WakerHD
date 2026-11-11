// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// A small CPU canvas for the VR view's own pictures (the item wheel, the wrist HUD): premultiplied RGBA floats, discs,
// rings, the game's item icons and text in a system font (stb_truetype, the copy bundled with Dear ImGui). The pictures
// are drawn only when their contents change, never per frame.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace vr::canvas {

struct Colour { float r, g, b, a; };
struct Canvas {
    int w = 0, h = 0;
    int n = 0;              // the side of a square canvas (w)
    std::vector<float> px;  // premultiplied
    explicit Canvas(int size) : Canvas(size, size) {}
    Canvas(int width, int height) : w(width), h(height), n(width), px(size_t(width) * height * 4, 0.0f) {}
    // `c` (straight alpha) over the pixel, with coverage k
    void blend(int x, int y, Colour c, float k) {
        if (x < 0 || y < 0 || x >= w || y >= h || k <= 0) return;
        float* p = &px[(size_t(y) * w + x) * 4];
        const float a = std::clamp(c.a * k, 0.0f, 1.0f);
        p[0] = c.r * a + p[0] * (1 - a);
        p[1] = c.g * a + p[1] * (1 - a);
        p[2] = c.b * a + p[2] * (1 - a);
        p[3] = a + p[3] * (1 - a);
    }
    // a disc (soft edge of a pixel) or a ring of `width` around radius r
    void disc(float cx, float cy, float r, Colour c) {
        for (int y = int(cy - r - 2); y <= int(cy + r + 2); ++y)
            for (int x = int(cx - r - 2); x <= int(cx + r + 2); ++x) {
                const float d = std::hypot(float(x) + 0.5f - cx, float(y) + 0.5f - cy);
                blend(x, y, c, std::clamp(r - d + 0.5f, 0.0f, 1.0f));
            }
    }
    void ring(float cx, float cy, float r, float width, Colour c) {
        const float outer = r + width * 0.5f, inner = r - width * 0.5f;
        for (int y = int(cy - outer - 2); y <= int(cy + outer + 2); ++y)
            for (int x = int(cx - outer - 2); x <= int(cx + outer + 2); ++x) {
                const float d = std::hypot(float(x) + 0.5f - cx, float(y) + 0.5f - cy);
                blend(x, y, c, std::clamp(std::min(outer - d, d - inner) + 0.5f, 0.0f, 1.0f));
            }
    }
    // a 128 x 128 RGBA icon (straight alpha) scaled to `size`, centred at cx, cy (bilinear), its alpha times `fade`
    void icon(const std::vector<uint8_t>& rgba, int iw, int ih, float cx, float cy, float size, float fade) {
        const int w = iw, h = ih;  // (the icon's)
        const float x0 = cx - size * 0.5f, y0 = cy - size * 0.5f, k = float(w) / size;
        for (int y = int(y0); y <= int(y0 + size); ++y)
            for (int x = int(x0); x <= int(x0 + size); ++x) {
                const float u = (float(x) + 0.5f - x0) * k - 0.5f, v = (float(y) + 0.5f - y0) * k - 0.5f;
                if (u < -0.5f || v < -0.5f || u > float(w) - 0.5f || v > float(h) - 0.5f) continue;
                const int iu = std::clamp(int(std::floor(u)), 0, w - 1), iv = std::clamp(int(std::floor(v)), 0, h - 1);
                const int ju = std::min(iu + 1, w - 1), jv = std::min(iv + 1, h - 1);
                const float fu = std::clamp(u - float(iu), 0.0f, 1.0f), fv = std::clamp(v - float(iv), 0.0f, 1.0f);
                float s[4] = {};
                for (int ch = 0; ch < 4; ++ch) {
                    auto t = [&](int a, int b) { return float(rgba[(size_t(b) * w + a) * 4 + ch]) / 255.0f; };
                    // premultiplied while filtering (no dark fringes at the icon's edge)
                    auto pm = [&](int a, int b) { return ch == 3 ? t(a, b) : t(a, b) * float(rgba[(size_t(b) * w + a) * 4 + 3]) / 255.0f; };
                    s[ch] = (pm(iu, iv) * (1 - fu) + pm(ju, iv) * fu) * (1 - fv) + (pm(iu, jv) * (1 - fu) + pm(ju, jv) * fu) * fv;
                }
                if (s[3] <= 0) continue;
                blend(x, y, {s[0] / s[3], s[1] / s[3], s[2] / s[3], fade}, s[3]);
            }
    }
};

// the typeface: the system font (Ui), or the Free Roam label's (Label: fonts/AngillaTattoo_PERSONAL_USE_ONLY.ttf next to
// the executable, a personal-use font kept out of the repository; the system font when it is not there)
enum class Face { Ui, Label };
// text: one line centred on cx, its middle at cy (ASCII), its width, and up to two lines within `width`. `bold`
// thickens the strokes by that many pixels (drawn again around itself: the label's font has no bold of its own)
float text_width(const std::string& s, float height, Face face = Face::Ui);
void text(Canvas& c, const std::string& s, float cx, float cy, float height, Colour col, Face face = Face::Ui,
          float bold = 0.0f);
std::vector<std::string> wrap(const std::string& s, float width, float height);
bool have_font();
// the game's item icons (frames 0..54, 128 x 128 RGBA; item_icons::game_icons), read once on a worker thread: asks for
// them (again at most every 5 s while the game has not read its pack yet), then calls `use` (if given) with them under
// their lock. Returns the loading state (0 not asked, 1 reading, 2 read): a picture is drawn again when it changes
int with_icons(void (*use)(const std::vector<std::vector<uint8_t>>& icons, void* ctx), void* ctx);
// the canvas as 8-bit premultiplied RGBA (or BGRA), rows top-down, into `out`
void to_pixels(const Canvas& c, bool bgra, std::vector<uint8_t>& out);

}  // namespace vr::canvas

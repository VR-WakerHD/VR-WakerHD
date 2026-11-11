// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// The VR view's CPU canvas (vr_canvas.h): its font, text and the game's item icons.
#include "vr_canvas.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <thread>

#include "mods/item_icons.h"
#include "platform/host.h"
#include "runtime.h"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"

namespace vr::canvas {
namespace {

// ---- the game's icons (frames 0..54, 128 x 128 RGBA), read once on a worker thread
std::mutex g_iconMu;
std::vector<std::vector<uint8_t>> g_icons;
std::atomic<int> g_iconState{0};  // 0 not asked, 1 reading, 2 read (possibly empty: no pack yet)

void ask_icons() {
    // read but empty (the game had not read its pack yet): asked again, at most every 5 s
    static std::chrono::steady_clock::time_point lastTry{};
    if (g_iconState.load() == 2) {
        std::lock_guard<std::mutex> lock(g_iconMu);
        const auto now = std::chrono::steady_clock::now();
        if (!g_icons.empty() || now - lastTry < std::chrono::seconds(5)) return;
        g_iconState = 0;
    }
    int expected = 0;
    if (!g_iconState.compare_exchange_strong(expected, 1)) return;
    lastTry = std::chrono::steady_clock::now();
    std::thread([] {
        std::vector<std::vector<uint8_t>> icons = item_icons::game_icons();
        {
            std::lock_guard<std::mutex> lock(g_iconMu);
            g_icons = std::move(icons);
        }
        g_iconState = 2;
    }).detach();
}

// ---- the font (the overlay's list: the first that exists)
struct Font {
    std::vector<unsigned char> file;
    stbtt_fontinfo info{};
    bool ok = false;
};
bool load_font(Font& out, const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.file.assign(std::istreambuf_iterator<char>(in), {});
    if (!out.file.empty() && stbtt_InitFont(&out.info, out.file.data(), stbtt_GetFontOffsetForIndex(out.file.data(), 0))) {
        out.ok = true;
        return true;
    }
    out.file.clear();
    return false;
}
const Font& font();
// the Free Roam label's font: fonts/AngillaTattoo_PERSONAL_USE_ONLY.ttf next to the executable (Angilla Tattoo by Mans
// Greback, free for personal use only: it is not in the repository, the user's own build loads it); the system font
// when it is not there
const Font& label_font() {
    static Font f = [] {
        Font out;
        const std::string p = host::exe_dir() + "/fonts/AngillaTattoo_PERSONAL_USE_ONLY.ttf";
        if (load_font(out, p)) LOG("[vr] VR pictures: the Free Roam label in %s", p.c_str());
        else LOG("[vr] VR pictures: no %s, the Free Roam label in the system font", p.c_str());
        return out;
    }();
    return f.ok ? f : font();
}
const Font& face_font(Face face) { return face == Face::Label ? label_font() : font(); }
const Font& font() {
    static Font f = [] {
        Font out;
        static const char* const paths[] = {
            "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
            "/System/Library/Fonts/Supplemental/Arial.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
            "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
            "/usr/share/fonts/noto/NotoSans-Regular.ttf"};
        for (const char* p : paths) {
            if (!load_font(out, p)) continue;
            LOG("[vr] VR pictures: text in %s", p);
            break;
        }
        if (!out.ok) LOG("[vr] VR pictures: no font found, they show icons without text");
        return out;
    }();
    return f;
}

}  // namespace

float text_width(const std::string& s, float height, Face face) {
    const Font& f = face_font(face);
    if (!f.ok) return 0;
    const float scale = stbtt_ScaleForPixelHeight(&f.info, height);
    float w = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&f.info, (unsigned char)s[i], &adv, &lsb);
        w += float(adv) * scale;
        if (i + 1 < s.size()) w += float(stbtt_GetCodepointKernAdvance(&f.info, (unsigned char)s[i], (unsigned char)s[i + 1])) * scale;
    }
    return w;
}
// one line centred on cx, its middle at cy (ASCII: the names are English)
void text(Canvas& c, const std::string& s, float cx, float cy, float height, Colour col, Face face, float bold) {
    const Font& f = face_font(face);
    if (!f.ok || s.empty()) return;
    // bold: each glyph's coverage spread over a disc of `bold` pixels (the most of the covering pixels: no darker
    // overlaps where the strokes meet)
    const int r = int(std::ceil(std::max(bold, 0.0f)));
    const float scale = stbtt_ScaleForPixelHeight(&f.info, height);
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&f.info, &ascent, &descent, &gap);
    const float baseline = cy + (float(ascent) + float(descent)) * scale * 0.5f;
    float x = cx - text_width(s, height, face) * 0.5f;
    for (size_t i = 0; i < s.size(); ++i) {
        const int cp = (unsigned char)s[i];
        int w = 0, h = 0, xo = 0, yo = 0;
        unsigned char* bmp = stbtt_GetCodepointBitmapSubpixel(&f.info, scale, scale, x - std::floor(x), 0, cp, &w, &h, &xo, &yo);
        if (bmp) {
            const int ox = int(std::floor(x)) + xo, oy = int(std::lround(baseline)) + yo;
            if (r == 0) {
                for (int y = 0; y < h; ++y)
                    for (int xx = 0; xx < w; ++xx) c.blend(ox + xx, oy + y, col, float(bmp[y * w + xx]) / 255.0f);
            } else {
                for (int y = -r; y < h + r; ++y)
                    for (int xx = -r; xx < w + r; ++xx) {
                        float k = 0;
                        for (int dy = -r; dy <= r; ++dy)
                            for (int dx = -r; dx <= r; ++dx) {
                                const int sx = xx + dx, sy = y + dy;
                                if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
                                const float d = std::hypot(float(dx), float(dy));
                                if (d > bold + 0.5f) continue;
                                const float edge = std::clamp(bold + 0.5f - d, 0.0f, 1.0f);
                                k = std::max(k, float(bmp[sy * w + sx]) / 255.0f * edge);
                            }
                        c.blend(ox + xx, oy + y, col, k);
                    }
            }
            stbtt_FreeBitmap(bmp, nullptr);
        }
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&f.info, cp, &adv, &lsb);
        x += float(adv) * scale;
        if (i + 1 < s.size()) x += float(stbtt_GetCodepointKernAdvance(&f.info, cp, (unsigned char)s[i + 1])) * scale;
    }
}
// up to two lines within `width` (split at a space)
std::vector<std::string> wrap(const std::string& s, float width, float height) {
    if (text_width(s, height) <= width) return {s};
    for (size_t cut = s.size(); cut-- > 0;)
        if (s[cut] == ' ' && text_width(s.substr(0, cut), height) <= width) return {s.substr(0, cut), s.substr(cut + 1)};
    return {s};
}

bool have_font() { return font().ok; }

int with_icons(void (*use)(const std::vector<std::vector<uint8_t>>& icons, void* ctx), void* ctx) {
    ask_icons();
    const int state = g_iconState.load();
    std::lock_guard<std::mutex> lock(g_iconMu);
    if (use) use(g_icons, ctx);
    return state;
}

void to_pixels(const Canvas& c, bool bgra, std::vector<uint8_t>& out) {
    const size_t count = size_t(c.w) * size_t(c.h);
    out.resize(count * 4);
    for (size_t i = 0; i < count; ++i) {
        const float* p = &c.px[i * 4];
        uint8_t* o = &out[i * 4];
        const uint8_t r = uint8_t(std::lround(std::clamp(p[0], 0.0f, 1.0f) * 255.0f));
        const uint8_t b = uint8_t(std::lround(std::clamp(p[2], 0.0f, 1.0f) * 255.0f));
        o[0] = bgra ? b : r;
        o[1] = uint8_t(std::lround(std::clamp(p[1], 0.0f, 1.0f) * 255.0f));
        o[2] = bgra ? r : b;
        o[3] = uint8_t(std::lround(std::clamp(p[3], 0.0f, 1.0f) * 255.0f));
    }
}

}  // namespace vr::canvas

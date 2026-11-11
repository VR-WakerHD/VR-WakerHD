// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// The VR aiming laser: vr_laser.h.
#include "vr_laser.h"

#include <algorithm>
#include <mutex>

namespace vr {
namespace {

std::mutex g_viewMu;
LaserView g_view;
std::vector<uint8_t> g_pixels[2];  // [bgra]

// one premultiplied pixel: colour (0..1) at alpha a
void put(std::vector<uint8_t>& px, int x, int y, float r, float g, float b, float a, bool bgra) {
    a = std::clamp(a, 0.0f, 1.0f);
    uint8_t* p = &px[(size_t(y) * kLaserWidth + size_t(x)) * 4];
    const uint8_t R = uint8_t(std::lround(std::clamp(r * a, 0.0f, 1.0f) * 255.0f));
    const uint8_t G = uint8_t(std::lround(std::clamp(g * a, 0.0f, 1.0f) * 255.0f));
    const uint8_t B = uint8_t(std::lround(std::clamp(b * a, 0.0f, 1.0f) * 255.0f));
    p[0] = bgra ? B : R;
    p[1] = G;
    p[2] = bgra ? R : B;
    p[3] = uint8_t(std::lround(a * 255.0f));
}

void draw(std::vector<uint8_t>& px, bool bgra) {
    px.assign(size_t(kLaserWidth) * kLaserHeight * 4, 0);
    // the beam: red across its width, brightest along its middle, the same all along
    for (int y = 0; y < kLaserBeamRect.h; ++y)
        for (int x = 0; x < kLaserBeamRect.w; ++x) {
            const float u = (x + 0.5f) / float(kLaserBeamRect.w) * 2.0f - 1.0f;  // -1 .. 1 across
            const float a = std::exp(-u * u * 6.0f) * 0.9f;
            const float core = std::exp(-u * u * 40.0f);
            put(px, kLaserBeamRect.x + x, kLaserBeamRect.y + y, 1.0f, 0.15f + 0.6f * core, 0.1f + 0.5f * core, a, bgra);
        }
    // the dots: round, soft-edged; the valid one bright red with a white core, the invalid one smaller and dim
    auto dot = [&](const LaserRect& r, float radius, float coreRadius, float alpha) {
        for (int y = 0; y < r.h; ++y)
            for (int x = 0; x < r.w; ++x) {
                const float dx = x + 0.5f - r.w * 0.5f, dy = y + 0.5f - r.h * 0.5f;
                const float d = std::sqrt(dx * dx + dy * dy);
                const float a = std::clamp(radius - d, 0.0f, 1.0f) * alpha;
                if (a <= 0) continue;
                const float w = coreRadius > 0 ? std::clamp(coreRadius - d, 0.0f, 1.0f) : 0.0f;
                put(px, r.x + x, r.y + y, 1.0f, 0.1f + 0.9f * w, 0.1f + 0.9f * w, a, bgra);
            }
    };
    dot(kLaserValidRect, 15.0f, 6.0f, 1.0f);
    dot(kLaserInvalidRect, 9.0f, 0.0f, 0.55f);
}

}  // namespace

void set_laser(const LaserView& v) {
    std::lock_guard<std::mutex> lock(g_viewMu);
    g_view = v;
}
bool laser_view(LaserView& out) {
    std::lock_guard<std::mutex> lock(g_viewMu);
    if (!g_view.visible) return false;
    out = g_view;
    return true;
}
const std::vector<uint8_t>& laser_pixels(bool bgra) {
    auto& px = g_pixels[bgra ? 1 : 0];
    if (px.empty()) draw(px, bgra);
    return px;
}

}  // namespace vr

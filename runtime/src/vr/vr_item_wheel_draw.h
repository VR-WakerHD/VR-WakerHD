// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// The VR item wheel's picture (docs/VR_ITEM_WHEEL.md): what the game thread shows (vr_game.cpp set_wheel) and the
// picture the render thread puts in its quad layer (vr_vulkan.cpp). The picture is drawn on the CPU once per change
// of its contents (the page, the selection, the inventory), not per frame.
#pragma once
#include <cstdint>
#include <vector>

#include "vr_item_wheel.h"
#include "vr_math.h"

namespace vr {

constexpr uint32_t kWheelPixels = 768;  // the picture's side (square)

struct WheelView {
    bool visible = false;
    Pose pose;              // tracking space; the picture faces +Z (towards the head)
    float size = 0.3f;      // metres (its side)
    uint64_t version = 0;   // changes with the contents (the picture is drawn again)
    std::vector<WheelPageItems> pages;
    std::vector<int> icons;  // per item number (256): the icon frame of BtnItemIcon_00, -1 none
    int page = 0, index = 0;
    int button = 0;          // the button the choice goes on (0 X, 1 Y, 2 R)
};

// game thread: what the wheel shows now
void set_wheel_view(const WheelView& v);
// render thread: the wheel as last set (false: not shown)
bool wheel_view(WheelView& out);
// render thread: the picture for `v` (kWheelPixels square, RGBA8 with premultiplied alpha, rows top-down), drawn again
// only when v.version changed; `bgra` swaps red and blue (a BGRA swapchain)
const std::vector<uint8_t>& wheel_pixels(const WheelView& v, bool bgra);

}  // namespace vr

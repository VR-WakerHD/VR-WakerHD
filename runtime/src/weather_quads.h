// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
#pragma once
// (VR fork) weather quads drawn as one draw: the game side (weather_quads.cpp), brackets of the weather packets' draws
namespace weather_quads {
bool enabled();  // Vulkan, and WWHD_VK_WEATHER_QUADS is not 0
// OP_WEATHER_QUADS 1 at construction and 0 at destruction (game thread), while enabled (recorded into a display list too)
struct Scope {
    Scope();
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    bool on;
};
}  // namespace weather_quads

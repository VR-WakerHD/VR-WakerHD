// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// (VR fork) Weather quads drawn as one draw (docs/VR_PERFORMANCE.md, "Weather quads").
//
// The weather packets (d_kankyo_wether.cpp in the decomp: dKankyo_*_Packet::render and friends) draw one quad a
// particle: a rain streak (mRainCount / 2 of them), 4 for each rain splash, a sparkle, a mist puff, a snowflake, a
// wave, a star... Each quad has its own vertex buffer (attribute buffer 0) and its own copy of the fetch shader; the
// splashes, sparkles, mist and sky clouds also have their own uniform blocks. In a rainy view that was 700 to 900
// draws a frame of one shader pair (WWHD_VK_INDEX_RUN_STATS=1: VS 003B8614), the biggest single cost of those views.
//
// These hooks bracket each of those draw functions with OP_WEATHER_QUADS (1 / 0). Between the two, gx2_core.cpp
// (weather runs) joins consecutive quads whose draw arguments are the same and whose only register changes are
// attribute buffer 0's address and the fetch shader's address (with the same fetch shader bytes): their vertices are
// copied one after another into a scratch ring, and the run is drawn as one draw with the indices stitched. The
// game's code and memory are untouched; outside the brackets nothing changes. Vulkan only;
// WWHD_VK_WEATHER_QUADS=0 turns it off.
#include <cstdlib>
#include <cstring>

#include "gfx/renderer.h"
#include "gx2/gx2_cmd.h"
#include "runtime.h"
#include "weather_quads.h"

namespace weather_quads {
bool enabled() {
    static const bool on = [] {
        const char* e = getenv("WWHD_VK_WEATHER_QUADS");
        const bool v = render::vulkan() && !(e && !strcmp(e, "0"));
        LOG("[weather] quads %s", v ? "of one weather draw drawn as one draw (WWHD_VK_WEATHER_QUADS=0 turns it off)"
                                    : "drawn one by one");
        return v;
    }();
    return on;
}
// (also while a display list records: the game records its weather draws into display lists and replays them; the
// markers are recorded with them and replayed in their order, as OP_GAMEPAD_BACKDROP's are)
Scope::Scope() : on(enabled()) {
    if (on) gx2::emit(gx2::OP_WEATHER_QUADS, {1u});
}
Scope::~Scope() {
    if (on) gx2::emit(gx2::OP_WEATHER_QUADS, {0u});
}
}  // namespace weather_quads

// the weather packets' quad-drawing functions (addresses from the decomp's d_kankyo_wether.cpp); the sky clouds'
// drawVrkumo (02575B6C) is bracketed in cloud_proto.cpp, which already hooks it
#define WEATHER_QUAD_HOOK(addr)                    \
    extern "C" void f_##addr##_orig(Cpu* c);       \
    extern "C" void hook_##addr(Cpu* c) {          \
        weather_quads::Scope scope;                \
        f_##addr##_orig(c);                        \
    }
WEATHER_QUAD_HOOK(0257E9DC)  // dKankyo_sun_Packet::render
WEATHER_QUAD_HOOK(0257EF00)  // dKankyo_sunlenz_Packet::render
WEATHER_QUAD_HOOK(0257F804)  // dKankyo_rain_Packet::renderSplashes
WEATHER_QUAD_HOOK(0257FB4C)  // dKankyo_rain_Packet::renderStreaks
WEATHER_QUAD_HOOK(02580870)  // dKankyo_snow_Packet::renderSplashes
WEATHER_QUAD_HOOK(02580B98)  // dKankyo_snow_Packet::renderParticles
WEATHER_QUAD_HOOK(02581060)  // dKankyo_star_Packet::setColor (its stars' draws)
WEATHER_QUAD_HOOK(025817B0)  // dKankyo_poison_Packet::render
WEATHER_QUAD_HOOK(02581C34)  // dKankyo_housi_Packet::render (the floating sparkles)
WEATHER_QUAD_HOOK(025820F4)  // dKankyo_cloud_Packet::render (cloud shadows)
WEATHER_QUAD_HOOK(02582650)  // dKankyo_wave_Packet::render

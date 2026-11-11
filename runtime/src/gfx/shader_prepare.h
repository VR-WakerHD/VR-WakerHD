// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
#pragma once
// (VR fork) Settings > Graphics (and VR) > Prepare shaders at start: before the game's first frame, the Vulkan
// warm-up builds every pipeline of the recipe database (the start queue and the recipes that otherwise wait for
// their object archives, as many as the pool of ready pipelines holds) on several threads, and the game waits for it
// (gx2_core.cpp GX2SwapScanBuffers), so first visits do not build them during play. On by default (a saved "0" turns
// it off); off: the warm-up runs in the background during the title screen, as before. Read at the start
// (gfx/vulkan/backend.cpp); a change applies at the next start. WWHD_PREPARE_SHADERS=1|0 wins for one start. Also in
// the VR launcher's Graphics tab (tools/vr_launcher). docs/vulkan.md, "Prepare shaders at start".
#include <atomic>
#include <cstddef>
#include <cstdlib>

namespace shaderprep {
constexpr const char* kSettingKey = "prepareShaders";  // "1" on, "0" off (settings.ini); missing: on
inline bool saved_on(bool found, const char* value) { return !found || value[0] != '0'; }
inline const char* env_override() {
    const char* e = std::getenv("WWHD_PREPARE_SHADERS");
    return e && *e ? e : nullptr;
}
inline std::atomic<bool> requested{false};  // this start prepares (the setting, or WWHD_PREPARE_SHADERS)
inline std::atomic<bool> active{false};     // the warm-up is preparing now: the game waits at its first frame
inline std::atomic<size_t> done{0}, total{0};  // pipelines prepared so far, of how many
}  // namespace shaderprep

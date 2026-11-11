// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// (VR fork) a start that tries VR (vr::wanted: the setting or --vr): the newer upstream control features
// (walking direction, camera swing, boat speed, item ring, touch talk, context buttons, one screen) stay off, so
// the VR controls work exactly as they did before the upstream merge
#pragma once
#ifdef WWHD_VR
#include "../vr/vr.h"
#endif

namespace mods {
inline bool vr_controls() {
#ifdef WWHD_VR
    static const bool on = vr::wanted();
    return on;
#else
    return false;
#endif
}
}  // namespace mods

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// The VR view's UI: the live conditions its decisions depend on (vr_ui.h, docs/VR_UI.md).
#include "vr_ui.h"

#include "vr.h"
#include "../aspect.h"

namespace vr::ui {

State state() {
    State s;
    s.vr = vr::active();
    s.firstPerson = s.vr && !vr::game::third_person_view();
    s.boatView = s.vr && vr::game::boat_view();
    s.shrunk = s.vr && vr::layouts_shrunk();
    s.telescope = s.vr && vr::game::telescope_looking();
    s.event = s.vr && !aspect::gameplay_or_conducting();
    s.noHeatHaze = s.vr && !vr::config().heatHaze;
    s.bombInHand = s.firstPerson && vr::game::bomb_in_hand();
    return s;
}

}  // namespace vr::ui

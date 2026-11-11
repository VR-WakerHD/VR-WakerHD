// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
#pragma once
// (docs/VR_DLSS.md, "Background motion and history resets") the motion pass's decisions that need no Vulkan, kept apart
// for the tests (motion_6dof_test): which draws give the background its motion, and when the temporal histories start
// again.
#include "../stereo_projection.h"
#include "motion_store.h"
#include <cmath>
#include <cstdint>
#include <cstring>

namespace motion {

// the draw has a 3D camera: a perspective projection (stereo::camera_projection, the same test the eye shift uses) in
// its vertex stage's data, at a 16-byte row. A layout, a full-screen effect or a 2D overlay has none
inline bool camera_draw(const DrawData& d) {
    for (const DrawData::Piece& p : d.pieces) {
        if (p.referred() || (p.part != Part::VertexSupport && p.part != Part::VertexBlock) || p.size < 64) continue;
        for (uint32_t at = 0; at + 64 <= p.size; at += 16) {
            float m[16];
            std::memcpy(m, d.bytes.data() + p.offset + at, 64);
            if (stereo::camera_projection(m)) return true;
        }
    }
    return false;
}

// the depth a background draw is pinned to in the motion pass (the value the frame's depth was cleared to): its
// fragments then pass the pass's EQUAL test only where no geometry wrote depth. Vulkan's viewport takes [0, 1]
inline bool background_depth_ok(float cleared) { return std::isfinite(cleared) && cleared >= 0.0f && cleared <= 1.0f; }

// a jump of the drawn view no head or walk makes in one frame (a recentre, a snap turn, a switch to an event camera,
// a teleport): the temporal histories start again. Turning: a head turns well under 10 degrees in a frame at 72 Hz or
// more, a snap turn is 30 to 90. Moving: a head or the boat at its fastest moves centimetres a frame
constexpr float kCutTurnRadians = 25.0f * 3.14159265f / 180.0f;
constexpr float kCutMoveMetres = 3.0f;
inline bool view_jump(float movedUnits, float turnedRadians, float unitsPerMetre) {
    if (!std::isfinite(movedUnits) || !std::isfinite(turnedRadians)) return true;
    const float metres = unitsPerMetre > 0.0f ? movedUnits / unitsPerMetre : 0.0f;
    return turnedRadians > kCutTurnRadians || metres > kCutMoveMetres;
}

// a new scene (a stage or room loaded, a cutscene's other set): most of an eye's draws found no draw of the previous
// frame to take their earlier positions from. A handful of new draws (an effect appearing) is not one
constexpr uint32_t kSceneMinDraws = 50;
inline bool scene_changed(uint32_t drawn, uint32_t unmatched) {
    return drawn >= kSceneMinDraws && unmatched * 2 > drawn;
}

// the motion target holds half the clip-space difference (previous minus current, x and y of the vertex shader's
// clip positions): a share of the picture along clip space's axes. In pixels that is the draw's own viewport size,
// its sign included: a viewport of negative height (the game's y scale negative, y up in clip space and down in the
// picture) turns y's sign. An eye's draws share one orientation; the majority decides
inline bool viewport_flips_y(uint32_t drawn, uint32_t negativeHeight) { return negativeHeight * 2 > drawn; }
inline float motion_scale_y(uint32_t height, bool flipsY) { return flipsY ? -float(height) : float(height); }

}  // namespace motion

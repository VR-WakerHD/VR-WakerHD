// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mehrab Kooner. VR mode by Mehrab Kooner (VR-WakerHD, https://github.com/mkooner):
// credit required, see LICENSE-VR.md.
// VR locomotion (docs/VR_CONTROLS.md): turning and the left stick.
//
// Link keeps moving with the game's own movement code. The game reads the left stick relative to its own
// camera's heading (the follow camera behind Link), which in VR is not where the player looks: the stick
// is turned by the difference of the two headings before the game reads it, so "forward" is the head's
// (or the left hand's) forward. Turning changes only the heading the tracking space faces in the game
// world (WorldMapping::heading); the game camera's logic is left alone.
#pragma once
#include <cmath>
#include "vr_config.h"
#include "vr_math.h"

namespace vr {

// right of a game heading: an actor facing heading_dir(h) has its right hand towards this
inline Vec3 heading_right(float h) { return {-std::cos(h), 0, std::sin(h)}; }

// the stick (x right, y forward; -1..1) the game must read so that, with its camera at cameraHeading,
// Link moves where the same stick means at wantedHeading
struct Stick {
    float x = 0, y = 0;
};
inline Stick remap_stick(Stick s, float wantedHeading, float cameraHeading) {
    const Vec3 world = heading_right(wantedHeading) * s.x + heading_dir(wantedHeading) * s.y;
    return {dot(world, heading_right(cameraHeading)), dot(world, heading_dir(cameraHeading))};
}

// turning with the right stick's x: snap turns (one per push past kSnapPress, released below
// kSnapRelease) or smooth turns. Positive stick x turns right, which lowers the game heading.
class Turner {
public:
    static constexpr float kSnapPress = 0.7f, kSnapRelease = 0.35f, kDeadZone = 0.2f;
    // returns the change of heading (radians) for this update
    float update(float stickX, float dt, const Config& c) {
        if (c.turn == Turn::Snap) {
            if (!armed_ && std::fabs(stickX) < kSnapRelease) armed_ = true;
            if (armed_ && std::fabs(stickX) >= kSnapPress) {
                armed_ = false;
                return -std::copysign(c.snapDegrees * kPi / 180.0f, stickX);
            }
            return 0;
        }
        armed_ = true;
        if (std::fabs(stickX) < kDeadZone || !(dt > 0)) return 0;
        const float t = (std::fabs(stickX) - kDeadZone) / (1.0f - kDeadZone);
        return -std::copysign(t * c.smoothDegreesPerSecond * kPi / 180.0f * std::fmin(dt, 0.1f), stickX);
    }

private:
    bool armed_ = true;
};

// the head's offset from the anchor (tracking space, metres) held within maxHeadOffset horizontally, so the
// view cannot walk away from Link's body; vertical offsets (crouching, standing up) are not limited
inline Vec3 limit_head_offset(Vec3 offset, float maxHorizontal) {
    const float h = std::sqrt(offset.x * offset.x + offset.z * offset.z);
    if (h <= maxHorizontal || h <= 0) return offset;
    const float k = maxHorizontal / h;
    return {offset.x * k, offset.y, offset.z * k};
}

}  // namespace vr

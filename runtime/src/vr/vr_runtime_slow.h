// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VR-WakerHD (https://github.com/VR-WakerHD/). VR mode by VR-WakerHD:
// credit required, see LICENSE-VR.md.
// SteamVR's slow state on Linux (docs/VR_FRAME_RATE.md, "SteamVR's slow state"): judged from xrEndFrame's time.
//
// The first detector switched after kSwitch frames in a row over 8 ms. A SteamVR 2.19.4 run (2026-11-20) spent most of
// its time in a mixed form, xrEndFrame alternating about 3 and 10 ms frame to frame (the median 6 to 7.5 ms): never ten
// slow frames in a row, so it was not seen while Auto Mode judged the frame rate and went to 45 fps, and later it
// flipped between the states. Now the average of the last kFrames frames decides, with a gap between the two limits:
//   state A (good)   about 1.7 ms a frame, the odd long one: an average about 2 ms;
//   mixed            about 3 and 10 ms alternating: 6 to 7.5 ms;
//   state B (slow)   about 10.8 ms every frame.
// Slow from an average over kEnterMs, good again under kLeaveMs. Pure (vr_test.cpp: test_runtime_slow).
#pragma once
#include <cmath>

namespace vr {

struct RuntimeSlowDetector {
    static constexpr int kFrames = 20;
    static constexpr double kEnterMs = 5.0, kLeaveMs = 3.0;
    double ring[kFrames] = {};
    int count = 0, next = 0;
    double sum = 0;
    bool slow = false;

    // one real frame's xrEndFrame (ms): true when the state changed with it (slow says which)
    bool update(double endMs) {
        if (!std::isfinite(endMs) || endMs < 0) return false;
        if (count == kFrames) sum -= ring[next];
        else ++count;
        ring[next] = endMs;
        sum += endMs;
        next = (next + 1) % kFrames;
        if (count < kFrames) return false;  // (a full window first)
        const double avg = sum / kFrames;
        const bool was = slow;
        if (!slow && avg > kEnterMs) slow = true;
        else if (slow && avg < kLeaveMs) slow = false;
        return slow != was;
    }
    double average() const { return count ? sum / count : 0; }
    // a pause (out of play, a load): the window starts again; the state stays as it was
    void restart() { count = next = 0, sum = 0; }
    void reset() { *this = RuntimeSlowDetector{}; }
};

}  // namespace vr

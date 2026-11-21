// The virtual vsync's flip timing without game state (gx2_core.cpp uses it; runtime/tools/
// vsync_pacing_test.cpp tests it, ctest vsync_pacing).
//
// The game paces itself on the Wii U's vsync: a flip executes on a vsync at least `swap interval`
// vsyncs after the previous one, and the game waits until its flips have executed. The virtual
// vsync ticks on its own clock (59.94 Hz, or 2/4 times as fast for 120/240 fps interpolation). On a
// fixed grid, a frame that is ready a little after its vsync waits for the next one: a computer that
// draws 55 frames a second got 30 at the 60 fps limit (issue #125), as with double-buffered vsync.
//
// Here a flip that is ready late executes as soon as it is ready, not at the next vsync:
// - lateness under one 59.94 Hz vsync (16.7 ms, also at 120/240 fps) keeps the grid: the flip counts
//   as made on its vsync, so a frame that is a little late, and the sleep timers' wake-up delay, take
//   nothing from the next frames and the long-term rate stays exactly 59.94 Hz (119.88, 239.76). At
//   120/240 fps the logic passes, which take longer than an in-between pass, regularly swap several
//   ms after their vsync, and the in-between frames make up for it;
// - a flip later than that moves the grid: the clock is advanced so the flip's moment is a vsync,
//   and the next flip is due one interval after it. The rate becomes what the computer draws (a
//   frame never waits for a vsync it has missed), and the grid never lags more than one vsync
//   behind, so faster frames afterwards catch up with at most one vsync's worth of frames.
// Frames that are ready on time flip exactly on the grid, as before.
#pragma once
#include <algorithm>
#include <cstdint>

namespace vsync_pacing {

// The clock: ticks (quarter vsyncs) since the epoch, advanced by `shift` nanoseconds. Times are
// nanoseconds since the epoch on the vsync's clock.
struct Clock {
    int64_t period;      // one vsync (59.94 Hz: 16'683'333 ns)
    uint64_t perVsync;   // ticks per vsync (4)
    uint64_t index(int64_t t, int64_t shift) const { return t + shift <= 0 ? 0 : uint64_t(t + shift) * perVsync / uint64_t(period); }
    // the time tick starts (the first time index() returns it)
    int64_t time(uint64_t tick, int64_t shift) const { return int64_t((tick * uint64_t(period) + perVsync - 1) / perVsync) - shift; }
    int64_t granule_ns(uint64_t granule) const { return int64_t(granule * uint64_t(period) / perVsync); }
};

// A flip executes at time t (ready, and its due tick has come: t >= time(due)). Returns the tick
// it counts as executed on (the next flip is due `interval` vsyncs after it) and updates shift:
// less than a vsync (`period`, at every rate) late keeps the grid (due); later moves the grid so t
// is a granule boundary (granule: ticks per vsync at the current rate).
inline uint64_t flip_executed(const Clock& c, int64_t& shift, uint64_t due, int64_t t, uint64_t granule) {
    const int64_t late = t - c.time(due, shift);
    if (late < c.period) return due;
    // the next boundary after t, and the shift that makes it start at t
    const uint64_t at = (c.index(t, shift) / granule + 1) * granule;
    shift += c.time(at, shift) - t;
    return at;
}

// Step slots (paced frame interpolation at 60, 120 and 240 fps, issue #164). The game advances one logic
// step every two 59.94 Hz vsyncs (its swap interval) and draws in-between frames between steps. A
// step's first frame (its logic frame) is due on the step's slot, two vsyncs after the previous
// step's slot, whatever the in-between frames did: the step schedule is the vsync grid itself.
// - On time, every step lasts exactly two vsyncs on the grid (29.97 steps a second, as on the
//   console). The separate 30.000 Hz wall-clock schedule of v0.2.13 drifted against the 29.97 Hz
//   grid: for seconds at a time a logic pass slept past its frame's vsync, that frame flipped up to
//   a vsync late and the step's in-between frames bunched up to catch up (an uneven step every
//   step), and every few seconds an in-between frame was skipped where the drift used up the
//   planning slack.
// - A logic frame that flips late (a slow computer) keeps the schedule: the next slot is still two
//   vsyncs after this one, and the next step plans fewer in-between frames to catch up (the game
//   keeps its speed). The logic pass decides when it is too late for that (interp.cpp: it starts
//   more than a step after its time) and drops the slot; the flip itself only counts as too late
//   at two steps. (Its flip lags the pass by the frames queued for the GPU: with a 15 ms render
//   load at 240 fps, logic frames flipped 18-36 ms after their slots while the passes kept time,
//   and a one-step limit at the flip restarted the schedule every ~20 steps: 29.74 steps/s.) A new
//   schedule starts one frame after the flip.
// Slots are times on the vsync's clock, not ticks, so a move of the grid does not move them.
//
// The first granule boundary (vsync at the drawn rate) that starts at or after `slot`.
inline uint64_t slot_tick(const Clock& c, int64_t slot, int64_t shift, uint64_t granule) {
    const uint64_t tick = c.index(slot - 1, shift) + 1;  // (index: the last tick that starts at or before t)
    return (tick + granule - 1) / granule * granule;
}
// The tick a flip is due on: `due` (`swap interval` after the previous flip), and a step's logic
// frame not before its step's slot (slot < 0: none).
inline uint64_t step_due(const Clock& c, uint64_t due, int64_t slot, int64_t shift, uint64_t granule) {
    return slot < 0 ? due : std::max(due, slot_tick(c, slot, shift, granule));
}
// The frames a step whose slot is `slot` has before the next step's logic frame (at 40 fps, granule
// 6 ticks, steps of 8: 2 for one step in three, 1 for the others; 60/120/240 fps: 2, 4, 8).
inline int step_frames(const Clock& c, int64_t slot, int64_t step, int64_t shift, uint64_t granule) {
    const uint64_t a = slot_tick(c, slot, shift, granule), b = slot_tick(c, slot + step, shift, granule);
    return b > a ? int((b - a) / granule) : 1;
}
// A step's logic frame flipped at t; `slot`: its slot (or, without one, the time it was due). The
// next step's slot (`step`: two vsyncs; `frame`: one frame at the drawn rate). Two steps late or
// more starts a new schedule: the next step may follow one frame after it (a computer that takes
// longer than that for a logic pass runs the game at its own pace, without waiting for a slot on
// top).
inline int64_t next_slot(int64_t slot, int64_t t, int64_t step, int64_t frame) {
    return t - slot < 2 * step ? slot + step : t + frame;
}

// VR paced by the runtime (docs/VR_RUNTIME_PACING_PLAN.md): the grid follows the moments the runtime releases its
// frames (xrWaitFrame returns). The granule boundary nearest `t` is moved to start at `t` (shift updated); returns how
// far the boundaries moved (ns, later when positive), for the step slots, which are times on the clock and must move
// with them. Nearest: a release a little early or late moves the grid by that little, never by a frame.
inline int64_t anchor_grid(const Clock& c, int64_t& shift, int64_t t, uint64_t granule) {
    granule = std::max<uint64_t>(granule, 1);
    const uint64_t i = c.index(t, shift) / granule * granule;  // the boundary at or before t
    const int64_t a = c.time(i, shift), b = c.time(i + granule, shift);
    const int64_t at = t - a <= b - t ? a : b;
    const int64_t d = t - at;
    shift -= d;
    return d;
}

// The moment the runtime releases a frame, from its display time: display time minus a smoothed offset between the
// two clocks (the runtime's XrTime and the game's), so the anchor is as even as the runtime's display times and the
// game needs no time-conversion extension. A release found earlier than the offset says (the runtime let the game go
// sooner) is followed quickly, a later one slowly: a late return is often the game asking late (xrWaitFrame returns at
// once then), not the runtime's rhythm. A run of samples a quarter period off (a hitch, a new rhythm) restarts it.
struct ReleaseAnchor {
    bool have = false;
    double offset = 0;  // ns: display time minus release
    int outliers = 0;
    int64_t update(int64_t sample, int64_t period) {
        if (!have) {
            have = true;
            offset = double(sample);
            return sample;
        }
        const double d = double(sample) - offset;
        if (period > 0 && (d > 0.25 * double(period) || -d > 0.25 * double(period))) {
            if (++outliers >= 8) offset = double(sample), outliers = 0;
            return int64_t(offset);
        }
        outliers = 0;
        offset += d > 0 ? d / 4 : d / 16;
        return int64_t(offset);
    }
};

}  // namespace vsync_pacing

// Dynamic resolution decisions (runtime/src/dynres_core.h): levels, steps down when GPU-bound, no
// change when the CPU is the limit, hysteresis on the way up, the doubled hold after an undone step,
// skipped windows, the test budget and the GPU busy union.
#include "dynres_core.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace dynres;

static int failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); \
            failures++;                                                  \
        }                                                                \
    } while (0)

// a half-second window at `fps` frames a second with the GPU busy `share` of the time
static Window win(double fps, double share, double target = 30, double seconds = 0.5) {
    Window w;
    w.seconds = seconds;
    w.frames = int(fps * seconds + 0.5);
    w.busyMs = share * seconds * 1000.0;
    w.targetFps = target;
    return w;
}

int main() {
    // levels and the minimum
    CHECK(lowest_level(0) == 0);
    CHECK(lowest_level(1) == 0);
    CHECK(lowest_level(0.75f) == 2);
    CHECK(lowest_level(0.5f) == 4);
    CHECK(lowest_level(0.3f) == 4);
    CHECK(lowest_level(0.8f) == 1);  // the lowest level at or above the minimum
    CHECK(kLevels[4] == 0.5f && 1280 * kLevels[1] == 1120 && 720 * kLevels[3] == 450);

    // off: never changes
    {
        Controller c;
        c.reset(0);
        for (int i = 0; i < 20; i++) CHECK(!c.decide(win(20, 1.0)).changed);
        CHECK(c.factor() == 1.0f);
    }
    // GPU-bound: one level down, the next window is not judged, then another level, down to the minimum
    {
        Controller c;
        c.reset(lowest_level(0.75f));
        Decision d = c.decide(win(27, 0.97));
        CHECK(d.changed && c.level == 1);
        d = c.decide(win(27, 0.97));  // settling
        CHECK(!d.judged && c.level == 1);
        d = c.decide(win(27, 0.97));
        CHECK(d.changed && c.level == 2);
        c.decide(win(27, 0.97));
        d = c.decide(win(27, 0.97));
        CHECK(!d.changed && c.level == 2);  // the minimum
    }
    // far below the frame rate: still one level at a time (the renderers keep only neighbouring levels)
    {
        Controller c;
        c.reset(4);
        Decision d = c.decide(win(18, 0.99));
        CHECK(d.changed && c.level == 1);
        c.decide(win(18, 0.99));  // settling
        d = c.decide(win(18, 0.99));
        CHECK(d.changed && c.level == 2);
    }
    // the CPU is the limit (GPU mostly idle): no change; at the frame rate with a busy GPU: no change
    {
        Controller c;
        c.reset(4);
        for (int i = 0; i < 10; i++) CHECK(!c.decide(win(20, 0.5)).changed);
        for (int i = 0; i < 10; i++) CHECK(!c.decide(win(30, 0.95)).changed);
        CHECK(c.level == 0);
    }
    // going up: needs room at the next level for two windows and the hold (2 s) since the step down
    {
        Controller c;
        c.reset(4);
        c.decide(win(25, 0.99));  // down to 0.875 at t=0.5
        CHECK(c.level == 1);
        c.decide(win(30, 0.5));   // settling (t=1.0)
        Decision d = c.decide(win(30, 0.5));  // room 1 (t=1.5)
        CHECK(d.judged && !d.changed);
        d = c.decide(win(30, 0.5));  // room 2, but only 1.5 s since the step down
        CHECK(!d.changed);
        d = c.decide(win(30, 0.5));  // 2.0 s
        CHECK(d.changed && c.level == 0);
    }
    // no step up when the next level would not fit: 0.70 at 0.875 predicts 0.91 at 1x
    {
        Controller c;
        c.reset(4);
        c.decide(win(25, 0.99));
        for (int i = 0; i < 20; i++) c.decide(win(30, 0.70));
        CHECK(c.level == 1);
        // 0.60 predicts 0.78: up
        for (int i = 0; i < 4; i++) c.decide(win(30, 0.60));
        CHECK(c.level == 0);
    }
    // a step up undone within 4 s doubles the hold
    {
        Controller c;
        c.reset(4);
        c.decide(win(25, 0.99));  // down
        c.decide(win(30, 0.5));
        for (int i = 0; i < 3; i++) c.decide(win(30, 0.5));
        CHECK(c.level == 0 && c.hold == 2.0);
        c.decide(win(30, 0.5));    // settling after the step up
        c.decide(win(25, 0.99));   // down again 1 s after the step up
        CHECK(c.level == 1 && c.hold == 4.0);
        c.decide(win(30, 0.5));
        int windows = 0;
        while (c.level == 1 && windows < 40) { c.decide(win(30, 0.5)); windows++; }
        CHECK(windows >= 7);  // 4 s at half-second windows
        CHECK(c.level == 0);
    }
    // a long window (paused, loading) and too few frames are not judged
    {
        Controller c;
        c.reset(4);
        CHECK(!c.decide(win(25, 0.99, 30, 3.0)).judged);
        Window w = win(25, 0.99);
        w.frames = 2;
        CHECK(!c.decide(w).judged);
        Window none = win(25, 0.99);
        none.busyMs = 0;  // no GPU timing
        CHECK(!c.decide(none).judged && c.level == 0);
    }
    // test budget: GPU ms per frame against the budget, whatever the frame rate
    {
        Controller c;
        c.reset(4);
        Window w = win(60, 0.3);  // 5 ms per frame
        w.budgetMs = 4;
        CHECK(c.decide(w).changed && c.level == 1);
        c.decide(w);
        w.budgetMs = 2;  // 2.5x the budget: still one level
        CHECK(c.decide(w).changed && c.level == 2);
        w.budgetMs = 50;
        int windows = 0;
        while (c.level > 0 && windows < 100) { c.decide(w); windows++; }
        CHECK(c.level == 0);
    }
    // the GPU busy union: overlaps count once, gaps not at all
    {
        BusyUnion u;
        u.add(0, 10);
        u.add(5, 15);
        u.add(20, 30);
        u.add(22, 25);  // inside an earlier interval
        CHECK(u.take() == 25);
        CHECK(u.take() == 0);
        u.add(40, 35);  // empty
        CHECK(u.busyNs == 0);
        u.add(1, 2);  // slightly before the last end: already covered
        CHECK(u.busyNs == 0);
        u.add(5e9, 6e9);
        CHECK(u.take() == 1e9);
        u.add(1000, 3000);  // far before (a new clock): the end starts over
        CHECK(u.take() == 2000);
    }
    // VR profiles (vr/vr_dynres.h): the highest level, the GPU target and the settle time
    {
        CHECK(highest_level(0) == 0 && highest_level(1) == 0 && highest_level(0.875f) == 1 && highest_level(0.8f) == 2);
        CHECK(highest_level(0.5f) == 4 && highest_level(0.3f) == 4);
        // a maximum under 1: starts at it and never goes above it
        Controller c;
        c.reset(lowest_level(0.5f), highest_level(0.75f));
        CHECK(c.level == 2 && c.factor() == 0.75f);
        for (int i = 0; i < 40; i++) c.decide(win(90, 0.10, 90));  // plenty of room
        CHECK(c.level == 2);
        // a tighter GPU target goes down where the default would not (85 % busy, missing 90 fps)
        Controller def, tight;
        def.reset(lowest_level(0.5f));
        tight.tune(0.80, 2.0);
        tight.reset(lowest_level(0.5f));
        CHECK(tight.downShare == 0.80 && std::fabs(tight.upShare - 0.70) < 1e-9 && tight.holdBase == 2.0);
        CHECK(!def.decide(win(80, 0.85, 90)).changed && def.level == 0);
        CHECK(tight.decide(win(80, 0.85, 90)).changed && tight.level == 1);
        // the tuning is kept across a reset; out-of-range values are clamped
        tight.reset(lowest_level(0.5f));
        CHECK(tight.downShare == 0.80);
        Controller wild;
        wild.tune(1.5, 100.0);
        CHECK(wild.downShare == 0.98 && wild.holdBase == Controller::kHoldMax);
        // a longer settle time: no step up before it has passed since the step down
        Controller slow;
        slow.tune(0.90, 4.0);
        slow.reset(lowest_level(0.5f));
        CHECK(slow.decide(win(80, 0.97, 90)).changed && slow.level == 1);
        slow.decide(win(90, 0.30, 90));  // settling
        for (int i = 0; i < 4; i++) slow.decide(win(90, 0.30, 90));  // 2 s of room: still within the 4 s
        CHECK(slow.level == 1);
        for (int i = 0; i < 6; i++) slow.decide(win(90, 0.30, 90));
        CHECK(slow.level == 0);
        // the range moved under a level in use: the level follows it
        Controller r;
        r.reset(lowest_level(0.5f));
        r.level = 4;
        r.set_range(lowest_level(0.75f), 0);
        CHECK(r.level == 2);
        r.set_range(4, 3);
        CHECK(r.level == 3);
    }
    // VR at half the headset's rate: quarter-second windows (36 fps at 72 Hz, 45 at 90); full rate, the flat game
    // (hz 0) and 30 fps outside VR keep half a second; the way up needs the same second of room either way
    {
        CHECK(window_seconds(36, 72) == kHalfRateWindowSeconds && window_seconds(45, 90) == kHalfRateWindowSeconds);
        CHECK(window_seconds(60, 120) == kHalfRateWindowSeconds);
        CHECK(window_seconds(72, 72) == kWindowSeconds && window_seconds(90, 90) == kWindowSeconds);
        CHECK(window_seconds(30, 0) == kWindowSeconds && window_seconds(60, 0) == kWindowSeconds);
        CHECK(room_windows(kWindowSeconds) == 2 && room_windows(kHalfRateWindowSeconds) == 4);
        // an overload at 36 fps found in a quarter second
        Controller c;
        c.reset(lowest_level(0.625f));
        c.roomWindows = room_windows(kHalfRateWindowSeconds);
        Decision d = c.decide(win(30, 0.97, 36, 0.25));
        CHECK(d.changed && c.level == 1);
        // back up: settling, then four quarter windows of room (a second, as two half-second ones) after the hold
        c.decide(win(36, 0.3, 36, 0.25));  // (not judged)
        for (int i = 0; i < 12; i++) c.decide(win(36, 0.3, 36, 0.25));  // (the 2 s hold passes)
        int windows = 0;
        while (c.level == 1 && windows < 10) {
            c.decide(win(36, 0.3, 36, 0.25));
            ++windows;
        }
        CHECK(c.level == 0);
        Controller h;  // (the same room in two half-second windows takes two of them)
        h.reset(lowest_level(0.625f));
        h.decide(win(30, 0.97, 36));
        h.decide(win(36, 0.3, 36));
        for (int i = 0; i < 6; i++) h.decide(win(36, 0.3, 36));
        int halves = 0;
        while (h.level == 1 && halves < 10) {
            h.decide(win(36, 0.3, 36));
            ++halves;
        }
        CHECK(h.level == 0);
    }
    // memory (MemoryGuard): line 384 MB, lowest level 4 (0.5x)
    {
        MemoryGuard m;
        // unknown headroom or plenty: nothing changes
        CHECK(m.step(0.5, -1, 384, 0, 4) == 0 && m.cap == 0);
        CHECK(m.step(0.5, 4000, 384, 2, 4) == 2 && m.cap == 0);
        // under the line: one level down, at most every second, and that is the upper limit
        CHECK(m.step(0.5, 300, 384, 0, 4) == 1 && m.cap == 1);
        CHECK(m.step(0.5, 300, 384, 1, 4) == 1);  // half a second later: not yet
        CHECK(m.step(0.6, 300, 384, 1, 4) == 2 && m.cap == 2);
        // far under (under half the line): the lowest level at once
        CHECK(m.step(0.1, 100, 384, 2, 4) == 4 && m.cap == 4);
        CHECK(m.step(1.5, 100, 384, 4, 4) == 4);  // at the lowest: stays
        CHECK(m.tight(500, 384) && !m.tight(768, 384) && !m.tight(-1, 384));
        // between the line and twice the line: the limit stays, the room does not count
        for (int i = 0; i < 200; i++) CHECK(m.step(0.5, 500, 384, 4, 4) == 4);
        CHECK(m.cap == 4);
        // above twice the line: after 60 s the limit goes up one level per 10 s
        double t = 0;
        while (m.cap == 4 && t < 120) { m.step(0.5, 1000, 384, 4, 4); t += 0.5; }
        CHECK(m.cap == 3 && t >= 60 && t <= 61);
        t = 0;
        while (m.cap == 3 && t < 60) { m.step(0.5, 1000, 384, 4, 4); t += 0.5; }
        CHECK(m.cap == 2 && t >= 9.5 && t <= 10.5);
        // a dip under twice the line starts the minute over
        m.step(0.5, 500, 384, 4, 4);
        t = 0;
        while (m.cap == 2 && t < 120) { m.step(0.5, 1000, 384, 4, 4); t += 0.5; }
        CHECK(m.cap == 1 && t >= 60);
        // a step down postpones going up again by a minute
        MemoryGuard d;
        d.step(2, 300, 384, 0, 4);
        CHECK(d.cap == 1);
        t = 0;
        while (d.cap == 1 && t < 200) { d.step(0.5, 1000, 384, 1, 4); t += 0.5; }
        CHECK(d.cap == 0 && t >= 60);
        // the current level below the limit stays; never above the limit
        MemoryGuard e;
        e.cap = 2;
        CHECK(e.step(0.5, 4000, 384, 3, 4) == 3 && e.step(0.5, 4000, 384, 1, 4) == 2);
    }
    // little memory: the level that keeps the 3D view at 1x at most
    {
        CHECK(MemoryGuard::one_x_level(1.0f, 4) == 0);
        CHECK(MemoryGuard::one_x_level(0.75f, 4) == 0);
        CHECK(MemoryGuard::one_x_level(1.5f, 4) == 3);  // 0.625 of 1.5x = 0.94x
        CHECK(1.5f * kLevels[MemoryGuard::one_x_level(1.5f, 4)] <= 1.0f);
        CHECK(MemoryGuard::one_x_level(2.0f, 4) == 4);
        CHECK(MemoryGuard::one_x_level(2.0f, 2) == 2);  // the lowest allowed level bounds it
        CHECK(MemoryGuard::one_x_level(1.125f, 4) == 1);
        MemoryGuard s;
        s.start_capped(2);
        CHECK(s.cap == 2 && s.step(0.5, 4000, 64, 2, 4) == 2);
        for (int i = 0; i < 1000; i++) s.step(0.5, 4000, 64, 2, 4);  // plenty of room for minutes: never above 1x
        CHECK(s.cap == 2 && s.floor == 2);
        s.step(2, 40, 64, 2, 4);  // memory low below the floor still steps down, and comes back to the floor only
        CHECK(s.cap == 3);
        for (int i = 0; i < 1000; i++) s.step(0.5, 4000, 64, 3, 4);
        CHECK(s.cap == 2);
    }
    // the controller does not step up above its upper limit
    {
        Controller c;
        c.reset(4);
        c.force(3);
        c.upperLimit = 3;
        Window w;
        w.seconds = 0.5;
        w.frames = 15;
        w.busyMs = 50;  // 10%: plenty of room
        for (int i = 0; i < 200; i++) c.decide(w);
        CHECK(c.level == 3);
        c.upperLimit = 0;
        for (int i = 0; i < 200; i++) c.decide(w);
        CHECK(c.level == 0);
    }
    if (failures) {
        std::fprintf(stderr, "dynres_test: %d failures\n", failures);
        return 1;
    }
    std::printf("dynres_test: ok\n");
    return 0;
}

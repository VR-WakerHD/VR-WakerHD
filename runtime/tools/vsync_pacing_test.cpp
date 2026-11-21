// The virtual vsync's flip timing (runtime/src/gx2/vsync_pacing.h) in a simulation of the game's
// frame loop: a frame starts when the previous flip has executed, is ready `cost` later, and flips
// at its vsync or, if it is late, as soon as it is ready. Checks that frames on time keep the exact
// 59.94 Hz grid (also with late wake-ups), that a computer slightly slower than the limit gets its
// own rate instead of half of it (issue #125), and that the grid never runs ahead in a burst; and
// the step slots of paced frame interpolation (issue #164; their pass loop: interp_pacing_test).
#include "../src/gx2/vsync_pacing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <vector>

using namespace vsync_pacing;

static int g_failed = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            g_failed++;                                                            \
        }                                                                          \
    } while (0)

static constexpr Clock kClock{16'683'333, 4};
static constexpr int64_t kMs = 1'000'000;

struct Run {
    std::vector<int64_t> flips;  // flip times (ns)
    double fps() const { return double(flips.size() - 1) * 1e9 / double(flips.back() - flips.front()); }
    int64_t max_interval() const {
        int64_t m = 0;
        for (size_t i = 1; i < flips.size(); i++) m = std::max(m, flips[i] - flips[i - 1]);
        return m;
    }
    int64_t min_interval() const {
        int64_t m = INT64_MAX;
        for (size_t i = 1; i < flips.size(); i++) m = std::min(m, flips[i] - flips[i - 1]);
        return m;
    }
};

// frames: cost(i) ns from the previous flip to the frame being ready; wake(i) = how late a wait for
// a vsync resumes (sleep timers). granule: ticks per vsync at the rate (4: 30/60 fps, 2: 120, 1: 240),
// interval: the game's swap interval in vsyncs at that rate. fixedGrid: the previous behaviour (a
// late frame waits for the next vsync).
static Run simulate(int frames, uint64_t granule, uint64_t interval, const std::function<int64_t(int)>& cost,
                    const std::function<int64_t(int)>& wake, bool fixedGrid = false, int64_t* maxLag = nullptr) {
    Run r;
    int64_t shift = 0, t = kClock.time(100 * granule, 0);  // the first flip, on a vsync
    uint64_t last = 100 * granule, lastIndex = 0;
    r.flips.push_back(t);
    for (int i = 0; i < frames; i++) {
        const int64_t ready = t + cost(i);
        const uint64_t due = last + interval * granule;
        int64_t flip;
        if (fixedGrid) {  // the first vsync at or after both
            uint64_t tick = std::max(due, (kClock.index(ready, shift) / granule + 1) * granule);
            flip = kClock.time(tick, shift) + wake(i);
            last = tick;
        } else {
            const int64_t dueTime = kClock.time(due, shift);
            flip = ready <= dueTime ? dueTime + wake(i) : ready;  // waits for the vsync, or flips when ready
            last = flip_executed(kClock, shift, due, flip, granule);
            if (maxLag) *maxLag = std::max(*maxLag, flip - kClock.time(last, shift));
        }
        const uint64_t index = kClock.index(flip, shift);
        CHECK(index >= lastIndex);  // the clock never runs backwards
        CHECK(index >= last);       // the flip's tick has started
        lastIndex = index;
        t = flip;
        r.flips.push_back(t);
    }
    return r;
}

static int64_t none(int) { return 0; }

static void test_on_time() {
    // fast frames (10 ms): exactly 59.94 Hz, every interval one vsync, also with wake-ups up to 1.75 ms
    // late (no drift: the grid is kept)
    std::mt19937 rng(7);
    std::uniform_int_distribution<int64_t> late(0, 1'750'000);
    std::vector<int64_t> wakes(2000);
    for (auto& w : wakes) w = late(rng);
    const Run a = simulate(2000, 4, 1, [](int) { return 10 * kMs; }, none);
    CHECK(std::fabs(a.fps() - 59.94) < 0.01);
    CHECK(a.max_interval() - a.min_interval() <= 1);
    const Run b = simulate(2000, 4, 1, [](int) { return 10 * kMs; }, [&](int i) { return wakes[i]; });
    CHECK(std::fabs(b.fps() - 59.94) < 0.01);
    // 30 fps (swap interval 2), 120 and 240 fps: their exact rates
    CHECK(std::fabs(simulate(1000, 4, 2, [](int) { return 20 * kMs; }, none).fps() - 29.97) < 0.01);
    CHECK(std::fabs(simulate(2000, 2, 1, [](int) { return 5 * kMs; }, none).fps() - 119.88) < 0.02);
    CHECK(std::fabs(simulate(2000, 1, 1, [](int) { return 3 * kMs; }, none).fps() - 239.76) < 0.05);
    // a frame that needs exactly one vsync (16.683 ms) is on time
    CHECK(std::fabs(simulate(1000, 4, 1, [](int) { return kClock.period; }, none).fps() - 59.94) < 0.01);
}

static void test_slightly_slow() {
    // issue #125: frames of 17.5 ms (57 fps uncapped). The fixed grid made every frame wait for the
    // next vsync (30 fps); now the computer's own rate.
    const auto slow = [](int) { return int64_t(17.5 * kMs); };
    const Run grid = simulate(1000, 4, 1, slow, none, true);
    CHECK(std::fabs(grid.fps() - 29.97) < 0.05);  // (the old behaviour, for reference)
    int64_t lag = 0;
    const Run now = simulate(1000, 4, 1, slow, none, false, &lag);
    CHECK(std::fabs(now.fps() - 1000.0 / 17.5) < 0.05);
    CHECK(now.max_interval() <= int64_t(17.5 * kMs));
    CHECK(lag < kClock.period);  // the grid follows within a vsync
    // 120 and 240 fps limits: the same rate (the reported ~50 fps there)
    CHECK(std::fabs(simulate(1000, 2, 1, slow, none).fps() - 1000.0 / 17.5) < 0.05);
    CHECK(std::fabs(simulate(1000, 1, 1, slow, none).fps() - 1000.0 / 17.5) < 0.05);
    // 30 fps with 40 ms frames: 25 fps (the grid: 20)
    CHECK(std::fabs(simulate(500, 4, 2, [](int) { return 40 * kMs; }, none).fps() - 25.0) < 0.05);
    CHECK(std::fabs(simulate(500, 4, 2, [](int) { return 40 * kMs; }, none, true).fps() - 19.98) < 0.05);
    // frames alternating 12 and 21 ms (average 16.5 ms, under one vsync): the slow ones flip late, the
    // fast ones make up for it on the grid, so the rate stays at about 59.94 and no frame waits for an
    // extra vsync
    const Run mixed = simulate(1000, 4, 1, [](int i) { return (i & 1 ? 21 : 12) * kMs; }, none);
    CHECK(mixed.fps() > 59.0);
    CHECK(mixed.max_interval() <= 21 * kMs);
}

static void test_hitch() {
    // one 100 ms hitch among 10 ms frames: the grid moves to the late flip, and the next frames come
    // one vsync apart (no burst of fast flips to catch up), then 59.94 Hz again
    const Run r = simulate(300, 4, 1, [](int i) { return (i == 100 ? 100 : 10) * kMs; }, none);
    CHECK(r.min_interval() >= kClock.period - 1);
    CHECK(r.max_interval() == 100 * kMs);
    CHECK(std::fabs(double(r.flips[300] - r.flips[102]) / 198 - double(kClock.period)) < 1.0);
    // a hitch of under one vsync late keeps the grid: the frames after it are back on their
    // vsyncs within two frames, and none waits for an extra vsync
    const Run s = simulate(300, 4, 1, [](int i) { return (i == 100 ? 24 : 10) * kMs; }, none);
    CHECK(s.flips[103] - s.flips[100] == kClock.time(112, 0) - kClock.time(100, 0));
    CHECK(s.max_interval() == 24 * kMs);
}

static void test_flip_executed() {
    int64_t shift = 0;
    // on time and less than a granule late: the due tick, the clock unchanged
    CHECK(flip_executed(kClock, shift, 40, kClock.time(40, 0), 4) == 40 && shift == 0);
    CHECK(flip_executed(kClock, shift, 40, kClock.time(40, 0) + kClock.period - 1, 4) == 40 && shift == 0);
    // later: the next granule boundary, which now starts at the flip
    const int64_t t = kClock.time(40, 0) + 20 * kMs;
    const uint64_t at = flip_executed(kClock, shift, 40, t, 4);
    CHECK(at == 48);
    CHECK(shift > 0 && shift < kClock.period);
    CHECK(kClock.time(at, shift) == t);
    CHECK(kClock.index(t, shift) == at);
    CHECK(kClock.index(t - 1, shift) == at - 1);
    // at 240 fps (granule 1) the same limit, and the next quarter-vsync boundary
    shift = 0;
    CHECK(flip_executed(kClock, shift, 40, kClock.time(40, 0) + 16 * kMs, 1) == 40);
    CHECK(flip_executed(kClock, shift, 40, kClock.time(40, 0) + 17 * kMs, 1) == 45 && shift > 0);
}


// Step slots (issue #164): a slot rounds up to the next vsync at the drawn rate, a step's logic frame
// waits for its slot, and the next slot is two vsyncs after this one unless the frame was two steps
// late.
static void test_step_slots() {
    const int64_t step = 2 * kClock.period, tick = kClock.granule_ns(1);
    // a slot on a tick is that tick; between ticks the next one (at 240 fps: quarter vsyncs; at 120
    // fps: half vsyncs)
    CHECK(slot_tick(kClock, kClock.time(40, 0), 0, 1) == 40);
    CHECK(slot_tick(kClock, kClock.time(40, 0) + 1, 0, 1) == 41);
    CHECK(slot_tick(kClock, kClock.time(40, 0) - 1, 0, 1) == 40);
    CHECK(slot_tick(kClock, kClock.time(41, 0), 0, 2) == 42);
    // with the grid moved (shift), on the moved grid
    const int64_t shift = 3 * kMs;
    CHECK(kClock.time(slot_tick(kClock, kClock.time(40, 0), shift, 1), shift) >= kClock.time(40, 0));
    CHECK(kClock.time(slot_tick(kClock, kClock.time(40, 0), shift, 1), shift) < kClock.time(40, 0) + tick);
    // a logic frame is due on its slot when the frame before it flipped earlier; an in-between
    // frame, or no slot: one interval after the previous flip
    CHECK(step_due(kClock, 35, kClock.time(40, 0), 0, 1) == 40);
    CHECK(step_due(kClock, 45, kClock.time(40, 0), 0, 1) == 45);
    CHECK(step_due(kClock, 35, -1, 0, 1) == 35);
    // two vsyncs = 8 ticks at 240 fps: the next slot is exactly the tick 8 later
    const int64_t slot = kClock.time(40, 0);
    CHECK(next_slot(slot, slot + 100'000, step, tick) == kClock.time(48, 0));
    CHECK(next_slot(slot, slot + step - 1, step, tick) == slot + step);  // late, but less than a step
    CHECK(next_slot(slot, slot + 2 * step - 1, step, tick) == slot + step);  // (the GPU queue's lag at the flip)
    CHECK(next_slot(slot, slot + 2 * step, step, tick) == slot + 2 * step + tick);  // two steps late: a new schedule
}


// VR paced by the runtime (docs/VR_RUNTIME_PACING_PLAN.md): the grid follows the runtime's releases
// (anchor_grid, ReleaseAnchor). The VR clock: a vsync of 1.5 refreshes of an 89.91 Hz headset, 12 ticks a
// vsync, a frame (granule) 8 ticks at 90 fps, 16 at 45.
static constexpr Clock kVr{16'683'350, 12};
static void test_anchor_grid() {
    const uint64_t g = 8;
    const int64_t frame = kVr.granule_ns(g);
    int64_t shift = 0;
    // a release 1 ms after a boundary: that boundary moves 1 ms later
    const int64_t b = kVr.time(800, 0);
    CHECK(anchor_grid(kVr, shift, b + 1 * kMs, g) == 1 * kMs);
    CHECK(kVr.time(800, shift) == b + 1 * kMs);
    CHECK(kVr.index(b + 1 * kMs, shift) == 800);
    // a release 1 ms before the next boundary: that one moves 1 ms earlier (nearest, never a frame)
    const int64_t next = kVr.time(808, shift);
    CHECK(anchor_grid(kVr, shift, next - 1 * kMs, g) == -1 * kMs);
    CHECK(kVr.time(808, shift) == next - 1 * kMs);
    // a release on a boundary: nothing moves
    const int64_t on = kVr.time(816, shift);
    CHECK(anchor_grid(kVr, shift, on, g) == 0);
    // the step slots move with the boundaries: the slot's tick stays the same
    const int64_t slot = kVr.time(824, shift) + 1000;  // (a slot just after a boundary: its tick is the next)
    const uint64_t tick = slot_tick(kVr, slot, shift, g);
    const int64_t d = anchor_grid(kVr, shift, kVr.time(824, shift) + 3 * kMs, g);
    CHECK(slot_tick(kVr, slot + d, shift, g) == tick);
    (void)frame;
}

// The runtime's releases against the game's grid over 30 s: the runtime's refresh 2 us longer than the headset's
// rate says (0.18 ms of drift a second), xrWaitFrame returning +-0.15 ms around each release, and in one frame of
// six the game asking 3 ms late (xrWaitFrame returns at once then). Without anchoring the grid drifts more than a
// refresh's tenth; anchored from the display times its boundaries stay on the releases.
static void test_runtime_releases() {
    const uint64_t g = 8;
    const int64_t refresh = 11'122'233 + 2'000;
    const int64_t clockOffset = 5'000'000'000'000;  // (the runtime's XrTime against the game's clock)
    std::mt19937 rng(7);
    std::uniform_int_distribution<int64_t> jitter(-150'000, 150'000);
    int64_t shift = 0, freeShift = 0;
    ReleaseAnchor anchor;
    const int64_t first = kVr.time(80, 0) + 4 * kMs;  // (the runtime's phase: 4 ms into the game's frame)
    int64_t worstLocked = 0, worstFree = 0;
    for (int k = 0; k < 2700; ++k) {
        const int64_t release = first + int64_t(k) * refresh;
        const int64_t display = release + 2 * refresh + clockOffset;
        int64_t returned = release + jitter(rng);
        if (k % 6 == 5) returned = release + 3 * kMs;  // (asked late: returns at once)
        const int64_t offset = anchor.update(display - returned, refresh);
        anchor_grid(kVr, shift, display - offset, g);
        if (k > 90) {  // (after a second to settle)
            const uint64_t i = kVr.index(release, shift) / g * g;
            const int64_t a = release - kVr.time(i, shift), b2 = kVr.time(i + g, shift) - release;
            worstLocked = std::max(worstLocked, std::min(a, b2));
            const uint64_t j = kVr.index(release, freeShift) / g * g;
            const int64_t fa = release - kVr.time(j, freeShift), fb = kVr.time(j + g, freeShift) - release;
            worstFree = std::max(worstFree, std::min(fa, fb));
        }
    }
    CHECK(worstLocked < 400'000);           // within 0.4 ms of every release (the late returns ignored)
    CHECK(worstFree > refresh / 10);        // (the game's own timer: drifted)
    // a new rhythm (the runtime's phase moves 5 ms, a hitch): locked again within 20 frames
    int64_t worstAfter = 0;
    for (int k = 0; k < 200; ++k) {
        const int64_t release = first + 5 * kMs + int64_t(3000 + k) * refresh;
        const int64_t display = release + 2 * refresh + clockOffset;
        const int64_t offset = anchor.update(display - release, refresh);
        anchor_grid(kVr, shift, display - offset, g);
        if (k >= 20) {
            const uint64_t i = kVr.index(release, shift) / g * g;
            const int64_t a = release - kVr.time(i, shift), b2 = kVr.time(i + g, shift) - release;
            worstAfter = std::max(worstAfter, std::min(a, b2));
        }
    }
    CHECK(worstAfter < 100'000);
}

int main() {
    test_flip_executed();
    test_step_slots();
    test_on_time();
    test_slightly_slow();
    test_hitch();
    test_anchor_grid();
    test_runtime_releases();
    if (g_failed) {
        std::fprintf(stderr, "vsync_pacing_test: %d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("vsync_pacing_test: ok\n");
    return 0;
}

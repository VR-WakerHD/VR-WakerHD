// Dynamic resolution: setting, GPU timing intake, frame-boundary decisions, log and report (dynres.h).
//
// Test switches (headless runs):
//   WWHD_DYNRES_MIN=0.5             the setting for this run (0 = off; the saved choice is not applied)
//   WWHD_DYNRES_TEST_BUDGET=f:ms,.. from render frame f on, judge against a GPU budget of ms per frame
//                                   instead of the frame rate (forces the scale down on a fast GPU);
//                                   ms 0 ends the test budget. Lf instead of f: f frames after the last
//                                   save state load (full: its load; portable: its arrival)
//   WWHD_DYNRES_TEST_AT=f:factor,.. from render frame f on (or Lf), force that level (nearest of the
//                                   levels); factor 0 hands back to the controller
//   WWHD_DYN_RES_MEM_MB=mb          the memory line (MemoryGuard, dynres_core.h): default 384, 64 under 3800 MB
//   WWHD_TEST_HEADROOM_MB=mb        stands for the room above the system's low-memory line (any system)
//   WWHD_TEST_TOTAL_MB=mb           stands for the device's memory (MemTotal; under 3800: starts at 1x at most)
//   WWHD_DYNRES_LOG_FRAMES=1        around each change, log the 8 frames before and after it: wall time
//                                   between frame boundaries and the render thread's CPU time (spikes)
//   WWHD_RES_FIXED=0.75             a fixed factor of the internal resolution for this run (0 = none; the
//                                   Quest's default otherwise, fixed_factor)
#include "dynres.h"
#include "dynres_core.h"
#include "interp.h"
#include "platform/memory_trim.h"
#include "savestate.h"
#include "render_prof.h"
#include "runtime.h"
#include "platform/quest_theater.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace dynres {
namespace {
using clk = std::chrono::steady_clock;

float clamp_min(float m) {
    if (!(m > 0.0f) || m >= 1.0f) return 0.0f;
    return kLevels[lowest_level(m)];
}
const char* env_min() { return getenv("WWHD_DYNRES_MIN"); }
std::atomic<float> g_min{env_min() ? clamp_min((float)atof(env_min())) : 0.0f};
// VR's profile (set_vr): under g_vr_mutex; g_vr_in/g_vr_on read without it
std::mutex g_vr_mutex;
struct VrProfile { float minScale = 0, maxScale = 1, gpuTarget = (float)Controller::kDownShare, settle = (float)Controller::kHoldBase; };
VrProfile g_vr;
std::atomic<bool> g_vr_in{false}, g_vr_on{false};
std::atomic<float> g_factor{1.0f};
std::atomic<float> g_base{1.0f};
std::atomic<float> g_share{-1.0f};
std::atomic<uint64_t> g_changes{0}, g_reused{0}, g_created{0};

std::mutex g_busy_mutex;
BusyUnion g_busy;  // under g_busy_mutex

// render thread: this frame's scale-change costs (dynres.h note_rescale), as text, then reset
std::string take_costs();
// test aid (WWHD_DYNRES_LOG_FRAMES): frame times around a change
struct FrameLog {
    const bool on = getenv("WWHD_DYNRES_LOG_FRAMES") != nullptr;
    static constexpr int kN = 8;
    double wall[kN]{}, cpu[kN]{};
    int next = 0, count = 0, after = 0;
    clk::time_point last{};
    uint64_t lastCpu = 0;
    void frame(uint64_t f, bool changed) {
        if (!on) return;
        const clk::time_point now = clk::now();
        const uint64_t c = rprof::thread_cpu_ns();
        if (last != clk::time_point{}) {
            const double w = std::chrono::duration<double, std::milli>(now - last).count(), u = (c - lastCpu) / 1e6;
            if (after > 0) {
                const std::string costs = take_costs();
                LOG("[dynres] frame %llu, +%d after the change: %.2f ms (render thread CPU %.2f ms)%s", (unsigned long long)f,
                    kN + 1 - after, w, u, costs.c_str());
                after--;
            } else {
                take_costs();
                wall[next] = w;
                cpu[next] = u;
                next = (next + 1) % kN;
                if (count < kN) count++;
            }
        }
        last = now;
        lastCpu = c;
        if (changed) {
            for (int i = 0; i < count; i++) {
                const int k = (next - count + i + kN) % kN;
                LOG("[dynres] frame -%d before the change: %.2f ms (render thread CPU %.2f ms)", count - i, wall[k], cpu[k]);
            }
            count = 0;
            after = kN;
        }
    }
};
FrameLog g_frame_log;
Controller g_ctl;
MemoryGuard g_mem;
clk::time_point g_mem_at{};
bool g_started = false;

// ---- memory (MemoryGuard): Android's room above its low-memory line
// /proc/meminfo's field in MB, -1 when not readable
int meminfo_mb(const char* field) {
#if defined(__ANDROID__) || defined(__linux__)
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[128];
    int mb = -1;
    const size_t n = strlen(field);
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, field, n) && line[n] == ':') {
            mb = (int)(atol(line + n + 1) >> 10);
            break;
        }
    fclose(f);
    return mb;
#else
    (void)field;
    return -1;
#endif
}
int env_int(const char* name, int fallback) { const char* e = getenv(name); return e ? atoi(e) : fallback; }
// devices with little memory (Tegra X1, 3 GB): start at 1x at most and use a lower line (they play with ~150 MB of
// room at the title). Only where the system gives a low-memory line (Android).
bool low_ram() {
    static const bool v = [] {
        const int t = env_int("WWHD_TEST_TOTAL_MB", -1) >= 0 ? env_int("WWHD_TEST_TOTAL_MB", -1)
#ifdef __ANDROID__
                                                          : meminfo_mb("MemTotal");
#else
                                                          : -1;
#endif
        return t > 0 && t < 3800;
    }();
    return v;
}
int mem_line() {
    static const int v = env_int("WWHD_DYN_RES_MEM_MB", low_ram() ? 64 : 384);
    return v;
}
// the room above the line: Java's reading (every 2 s, memory_trim.h) or, fresher, MemAvailable read here every
// 250 ms against Java's line, the lower of both (a load can eat 300 MB in 4 s); -1 unknown
int headroom(clk::time_point now) {
    static const int test = env_int("WWHD_TEST_HEADROOM_MB", -1);
    if (test >= 0) return test;
    const int h = memory_trim::headroom_mb(), thr = memory_trim::threshold_mb();
    static int fast = -1;
    static clk::time_point fastAt{};
    if (thr < 0) return h;
    if (std::chrono::duration<double>(now - fastAt).count() >= 0.25) {
        fastAt = now;
        const int avail = meminfo_mb("MemAvailable");
        fast = avail >= 0 ? std::max(0, avail - thr) : -1;
    }
    if (fast < 0) return h;
    return h < 0 ? fast : std::min(h, fast);
}
clk::time_point g_window{};
int g_frames = 0;
bool g_no_timing_logged = false;

struct Entry { uint64_t frame; bool afterLoad; float value; };
std::vector<Entry> schedule(const char* name) {
    std::vector<Entry> v;
    if (const char* e = getenv(name))
        for (char* p = (char*)e; *p;) {
            const bool afterLoad = *p == 'L';
            if (afterLoad) p++;
            uint64_t f = strtoull(p, &p, 10);
            if (*p++ != ':') break;
            v.push_back({f, afterLoad, (float)strtod(p, &p)});
            while (*p == ',') p++;
        }
    return v;
}
// the value of a frame schedule at `frame` (-1: no entry yet)
float scheduled(const std::vector<Entry>& s, uint64_t frame) {
    float v = -1;
    // the newer of the last full load and the last portable load's arrival
    const uint64_t loaded = std::max(ss::last_load_frame(), ss::last_arrival_frame());
    for (auto& e : s)
        if (e.afterLoad ? loaded && frame >= loaded + e.frame : frame >= e.frame) v = e.value;
    return v;
}
int nearest_level(float factor) {
    int best = 0;
    for (int i = 1; i < kLevelCount; i++)
        if (std::fabs(kLevels[i] - factor) < std::fabs(kLevels[best] - factor)) best = i;
    return best;
}
double target_fps() {
    // VR: the rate the headset is paced at (vr.frameRate: 90 at 90 Hz, 45 capped at half; unlocked: the refresh rate)
    if (const int hz = interp::vr_display_hz(); hz > 0)
        return interp::mode() == 1 ? std::min(interp::output_fps() > 0 ? interp::output_fps() : hz, hz) : 30;
    switch (interp::mode()) {
    case 1: return interp::output_fps() > 0 ? interp::output_fps() : 60;
    case 2: return 60;
    default: return 30;
    }
}
void log_change(float from, float to, float base, const Decision* d, double hold, const char* why, double budgetMs = 0) {
    char detail[160] = "";
    if (d && d->share >= 0 && budgetMs > 0)
        snprintf(detail, sizeof detail, ": GPU %.2f ms per frame against a %.2f ms budget; next step up after at least %.0f s",
                 d->share * budgetMs, budgetMs, hold);
    else if (d && d->share >= 0)
        snprintf(detail, sizeof detail, ": GPU busy %.0f%%, %.1f of %.0f fps; next step up after at least %.0f s", d->share * 100,
                 d->fps, target_fps(), hold);
    LOG("[dynres] %gx -> %gx of the %gx internal resolution (3D view %.0fx%.0f)%s%s", from, to, base, 1280 * base * to,
        720 * base * to, why, detail);
}
}  // namespace

void set_min(float m) {
    float v = clamp_min(m);
#ifdef __ANDROID__
    // Quest headsets and the Steam Frame (2026-10-31): off whatever the setting. Each change of scale makes the render targets again, and
    // with multiview their two-eye images are rebuilt: two Quest 3 crashes in the Adreno driver came right after one
    // (docs/VR_ANDROID.md). It cannot help there either: the CPU holds the Quest's frame rate, not the GPU (it went
    // down at 90 % GPU busy and back up at 55 %, the game at 24 to 30 fps throughout). WWHD_DYNRES_QUEST=1: as set
    static const bool quest = quest_theater::standalone_headset() && !std::getenv("WWHD_DYNRES_QUEST");
    if (quest && v > 0) {
        static bool logged = false;
        if (!logged) logged = true, LOG("[dynres] dynamic resolution off on a Quest or Steam Frame (the setting is kept; WWHD_DYNRES_QUEST=1 uses it)");
        v = 0;
    }
#endif
    if (g_min.exchange(v) != v) LOG("[dynres] dynamic resolution %s", v > 0 ? (v < 0.6f ? "on, down to 0.5x" : "on, down to 0.75x") : "off");
}
void set_vr(bool inVr, bool on, float minScale, float maxScale, float gpuTarget, float settleSeconds) {
    VrProfile p;
    if (inVr && on) {
        p.maxScale = std::clamp(maxScale, kLevels[kLevelCount - 1], 1.0f);
        p.minScale = std::clamp(minScale, kLevels[kLevelCount - 1], p.maxScale);
        p.gpuTarget = gpuTarget;
        p.settle = settleSeconds;
    }
    bool changed;
    {
        std::lock_guard<std::mutex> lock(g_vr_mutex);
        changed = g_vr_in.load() != inVr || g_vr_on.load() != (inVr && on) || g_vr.minScale != p.minScale ||
                  g_vr.maxScale != p.maxScale || g_vr.gpuTarget != p.gpuTarget || g_vr.settle != p.settle;
        g_vr = p;
        g_vr_on = inVr && on;
        g_vr_in = inVr;
    }
    if (!changed) return;
    if (!inVr) LOG("[dynres] VR session over: the Graphics setting again (%s)", g_min.load() > 0 ? "on" : "off");
    else if (!on) LOG("[dynres] VR: dynamic resolution off");
    else
        LOG("[dynres] VR: dynamic resolution %gx .. %gx of the internal resolution (levels %gx .. %gx), GPU target "
            "%.0f%% busy, settle %.1f s", p.minScale, p.maxScale, kLevels[lowest_level(p.minScale)],
            kLevels[highest_level(p.maxScale)], double(p.gpuTarget) * 100, double(p.settle));
}
// in VR its profile's minimum (a minimum of 1 with a maximum of 1: the full resolution only, which counts as off)
float min_factor() {
    if (g_vr_in.load(std::memory_order_relaxed)) {
        if (!g_vr_on.load(std::memory_order_relaxed)) return 0.0f;
        std::lock_guard<std::mutex> lock(g_vr_mutex);
        const int lo = lowest_level(g_vr.minScale), hi = highest_level(g_vr.maxScale);
        return lo > 0 || hi > 0 ? kLevels[std::max(lo, hi)] : 0.0f;
    }
    return g_min.load(std::memory_order_relaxed);
}
bool on() { return min_factor() > 0; }
bool min_from_env() { return env_min() != nullptr; }

// (watch_gpu) the busy time watched for the VR log, apart from dynamic resolution's own windows
std::atomic<bool> g_watch{false};
BusyUnion g_watched;  // under g_busy_mutex
void gpu_interval(double startNs, double endNs) {
    std::lock_guard<std::mutex> lock(g_busy_mutex);
    if (on()) g_busy.add(startNs, endNs);
    if (g_watch.load(std::memory_order_relaxed)) g_watched.add(startNs, endNs);
}
bool timing_wanted() { return on() || g_watch.load(std::memory_order_relaxed); }
void watch_gpu(bool on) {
    std::lock_guard<std::mutex> lock(g_busy_mutex);
    g_watch.store(on, std::memory_order_relaxed);
    g_watched = {};
}
double take_watched_ns() {
    std::lock_guard<std::mutex> lock(g_busy_mutex);
    return g_watched.take();
}

// A fixed factor of the internal resolution while dynamic resolution is off (asked for, 2026-10-31): the internal
// resolution's 0.5x and 0.75x (Graphics menu), one of the levels the renderers already draw at; by default 0.75 on a
// Quest or Steam Frame (the 3D view 960x540 of the 1x internal resolution's 1280x720, in VR the eye picture's height
// as tall), else none (1). It changes only when the setting does, so none of dynamic resolution's image swaps, kept
// images or neighbours made ahead (on() stays false, and with it multiview's two-eye targets). On a Quest or Frame the
// size drawn is the one at the first frame, a new choice at the next start: a rescale preceded the Adreno driver's
// crashes there. Shadow maps keep the internal resolution, as with dynamic resolution. WWHD_RES_FIXED=f sets it for a
// run (0: none; the setting is then not applied)
namespace {
float snap_fixed(float v) { return !(v > 0.0f) || v >= 1.0f ? 1.0f : kLevels[nearest_level(v)]; }
float default_fixed() {
    static const float f = [] {
        if (const char* e = getenv("WWHD_RES_FIXED")) return snap_fixed((float)atof(e));
#ifdef __ANDROID__
        if (quest_theater::standalone_headset()) return 0.75f;
#elif defined(WWHD_STEAM_FRAME_NATIVE) || (defined(__linux__) && defined(__aarch64__))
        // Steam Frame - Native (Linux arm64, 2026-11-03): the same 0.75 as a Quest and Frame - Lepton (its log drew the
        // eyes at the full 1280x1312, GPU-bound at 24-28 fps)
        return 0.75f;
#endif
        return 1.0f;
    }();
    return f;
}
std::atomic<float> g_fixed{-1.0f};  // the setting (-1: none chosen, the default)
bool fixed_frozen() {
#ifdef __ANDROID__
    static const bool headset = quest_theater::standalone_headset();
    return headset;
#else
    return false;
#endif
}
}  // namespace
std::atomic<float> g_dlss{1.0f};  // (set_dlss)
float fixed_factor() {
    if (const float d = g_dlss.load(std::memory_order_relaxed); d < 1.0f) return d;
    const float v = g_fixed.load(std::memory_order_relaxed);
    return v >= 0.0f && !getenv("WWHD_RES_FIXED") ? v : default_fixed();
}
void set_fixed(float factor) { g_fixed.store(snap_fixed(factor), std::memory_order_relaxed); }
void set_dlss(float factor) { g_dlss.store(snap_fixed(factor), std::memory_order_relaxed); }
bool fixed_applies_at_start() { return fixed_frozen(); }

float latch(uint64_t frame, float base) {
    g_base.store(base, std::memory_order_relaxed);
    const float before = g_ctl.factor();
    if (!on()) {
        if (g_started) {
            g_started = false;
            g_ctl.reset(0);
            g_mem = MemoryGuard{};
            g_share = -1;
            std::lock_guard<std::mutex> lock(g_busy_mutex);
            g_busy = {};
            if (before != 1.0f) log_change(before, 1.0f, base, nullptr, 0, " (dynamic resolution off)");
        }
        // (a Quest or Frame: the size at the first frame for the whole session)
        static const float atStart = fixed_factor();
        const float fixed = fixed_frozen() ? atStart : fixed_factor();
        static float logged = 1.0f;
        if (fixed != logged) {
            logged = fixed;
            LOG("[dynres] fixed %gx of the %gx internal resolution (3D view %.0fx%.0f)%s", fixed, base, 1280 * base * fixed,
                720 * base * fixed,
                getenv("WWHD_RES_FIXED") ? " (WWHD_RES_FIXED)" : g_fixed.load() >= 0 ? " (the setting)" : " (the headset's default)");
        }
        g_factor.store(fixed, std::memory_order_relaxed);
        return fixed;
    }
    const clk::time_point now = clk::now();
    // the range and tuning: VR's profile in a session, else the setting's (down to its minimum, from the full size)
    int lowest = lowest_level(min_factor()), highest = 0;
    double gpuTarget = Controller::kDownShare, settle = Controller::kHoldBase;
    if (g_vr_on.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(g_vr_mutex);
        highest = highest_level(g_vr.maxScale);
        lowest = std::max(highest, lowest_level(g_vr.minScale));
        gpuTarget = g_vr.gpuTarget;
        settle = g_vr.settle;
    }
    if (!g_started) {
        g_started = true;
        g_ctl.tune(gpuTarget, settle);
        g_ctl.reset(lowest, highest);
        g_mem = MemoryGuard{};
        g_mem_at = now;
        if (low_ram()) {
            const int l = MemoryGuard::one_x_level(base, g_ctl.lowest);
            if (l > 0) {
                g_mem.start_capped(l);
                g_ctl.force(l);
                LOG("[dynres] little memory: starts at %gx of the %gx internal resolution at most (MemoryGuard)", kLevels[l], base);
            }
        }
        g_window = now;
        g_frames = 0;
        std::lock_guard<std::mutex> lock(g_busy_mutex);
        g_busy = {};
    }
    g_ctl.set_range(lowest, highest);
    if (g_ctl.downShare != std::clamp(gpuTarget, 0.5, 0.98) || g_ctl.holdBase != std::clamp(settle, 0.5, Controller::kHoldMax))
        g_ctl.tune(gpuTarget, settle);
    static const auto forcedAt = schedule("WWHD_DYNRES_TEST_AT");
    static const auto budgetAt = schedule("WWHD_DYNRES_TEST_BUDGET");
    const float forced = scheduled(forcedAt, frame);
    // memory before the GPU: a step down for memory is taken at once, and limits the steps up
    {
        const double dt = std::chrono::duration<double>(now - g_mem_at).count();
        g_mem_at = now;
        const int h = headroom(now), line = mem_line();
        const int was = g_ctl.level;
        const int to = forced > 0 ? was : g_mem.step(dt, h, line, was, g_ctl.lowest);
        if (to != was) {
            g_ctl.force(to);
            LOG("[dynres] %gx -> %gx of the %gx internal resolution (memory low: %d MB above the low-memory line, under %d%s; "
                "upper limit %gx)", kLevels[was], kLevels[to], base, h, line, h < line / 2 ? ", far under: lowest" : "",
                kLevels[g_mem.cap]);
        }
        g_ctl.upperLimit = g_mem.tight(h, line) ? g_ctl.level : g_mem.cap;
        static int loggedCap = 0;
        if (g_mem.cap < loggedCap) LOG("[dynres] memory back (%d MB above the low-memory line): upper limit %gx", h, kLevels[g_mem.cap]);
        loggedCap = g_mem.cap;
    }
    g_frames++;
    double busyNs;
    {
        std::lock_guard<std::mutex> lock(g_busy_mutex);
        busyNs = g_busy.busyNs;
    }
    const double el = std::chrono::duration<double>(now - g_window).count();
    // (dynres_core.h window_seconds) a quarter-second window in VR at half the headset's rate, else half a second
    const double windowSeconds = window_seconds(target_fps(), interp::vr_display_hz());
    g_ctl.roomWindows = room_windows(windowSeconds);
    if (forced > 0) {
        g_ctl.force(nearest_level(forced));
    } else if (el >= windowSeconds) {
        {
            std::lock_guard<std::mutex> lock(g_busy_mutex);
            busyNs = g_busy.take();
        }
        Window w;
        w.seconds = el;
        w.busyMs = busyNs / 1e6;
        w.frames = g_frames;
        w.targetFps = target_fps();
        w.budgetMs = std::max(0.0f, scheduled(budgetAt, frame));
        g_window = now;
        g_frames = 0;
        const Decision d = g_ctl.decide(w);
        // the real busy share (in test-budget mode d.share is the ratio to the budget)
        if (w.busyMs > 0 && w.seconds > 0) g_share = (float)(w.busyMs / (w.seconds * 1000.0));
        if (w.busyMs <= 0 && !g_no_timing_logged && el < 2.0) {
            g_no_timing_logged = true;
            LOG("[dynres] no GPU timing from this renderer/device yet: the resolution stays until there is");
        }
        if (d.changed) log_change(before, g_ctl.factor(), base, &d, g_ctl.hold, w.budgetMs > 0 ? " (test budget)" : "", w.budgetMs);
    }
    const float after = g_ctl.factor();
    if (after != before) {
        g_changes++;
        if (forced > 0) log_change(before, after, base, nullptr, 0, " (test schedule)");
    }
    g_frame_log.frame(frame, after != before);
    g_factor.store(after, std::memory_order_relaxed);
    return after;
}

float factor() { return g_factor.load(std::memory_order_relaxed); }

std::string status() {
    char b[200];
    if (!on()) {
        const float fixed = fixed_factor(), base = g_base.load(std::memory_order_relaxed);
        if (fixed >= 1.0f) return "Dynamic resolution: off";
        snprintf(b, sizeof b, "Dynamic resolution: off; fixed %gx of %gx (3D view %.0fx%.0f)", fixed, base, 1280 * base * fixed,
                 720 * base * fixed);
        return b;
    }
    const float f = factor(), base = g_base.load(std::memory_order_relaxed), share = g_share.load(std::memory_order_relaxed);
    char busy[48] = "";
    if (share >= 0) snprintf(busy, sizeof busy, ", GPU busy %.0f%%", share * 100);
    snprintf(b, sizeof b, "Dynamic resolution: %gx of %gx (3D view %.0fx%.0f), down to %gx%s, %llu changes", f, base,
             1280 * base * f, 720 * base * f, min_factor(), busy, (unsigned long long)g_changes.load());
    return b;
}

int levels() { return on() ? lowest_level(min_factor()) + 1 : 1; }
float level_factor(int level) { return kLevels[std::clamp(level, 0, kLevelCount - 1)]; }

Stats stats() { return {g_changes.load(), g_reused.load(), g_created.load()}; }
void count_reused() { g_reused++; }
void count_created() { g_created++; }
// per frame (render thread): rescales and their cost, images made ahead
static struct { int rescales = 0, created = 0, precreated = 0, views = 0, images = 0; double createMs = 0, copyMs = 0, precreateMs = 0; } g_cost;
void note_rescale(bool created, double createMs, double copyMs) {
    g_cost.rescales++;
    g_cost.created += created;
    g_cost.createMs += createMs;
    g_cost.copyMs += copyMs;
}
void note_view() { g_cost.views++; }
void note_image() { g_cost.images++; }
void note_precreated(int images, double ms) {
    g_cost.precreated += images;
    g_cost.precreateMs += ms;
}
namespace {
std::string take_costs() {
    char b[200] = "";
    if (g_cost.rescales || g_cost.precreated || g_cost.views || g_cost.images)
        snprintf(b, sizeof b, "; %d targets rescaled, %d new images %.2f ms, copies %.2f ms; %d images/views made ahead %.2f ms; "
                 "all images created %d, views created %d", g_cost.rescales, g_cost.created, g_cost.createMs, g_cost.copyMs,
                 g_cost.precreated, g_cost.precreateMs, g_cost.images, g_cost.views);
    g_cost = {};
    return b;
}
}  // namespace

}  // namespace dynres

// Dynamic resolution (Settings > Graphics > Dynamic resolution): while the GPU holds the game below
// its frame rate, the renderers draw at a lower resolution (down to the chosen minimum, a fraction of
// the internal resolution); with room again they go back up. Decision logic: dynres_core.h.
// Off (the default) leaves everything as before: the factor is exactly 1 and no GPU timing is added.
// Idea: GreenNaugahyde's Android fork, v0.9.5.
#pragma once
#include <cstdint>
#include <string>

namespace dynres {

// the setting: the lowest factor of the internal resolution, 0 = off (0.75 or 0.5). Any thread.
void set_min(float factor);
// VR (vr/vr_dynres.h, docs/VR_DYNAMIC_RESOLUTION.md; the VR renderer, each headset frame it changes): while a session
// runs, its profile replaces the setting above: on with its range (of the internal resolution), GPU target (busy share
// counted as overloaded) and settle time (the first hold, s), or off. inVr false: the setting above again
void set_vr(bool inVr, bool on, float minScale, float maxScale, float gpuTarget, float settleSeconds);
float min_factor();
bool on();
bool min_from_env();  // WWHD_DYNRES_MIN chose it for this run (the saved setting is not applied)

// GPU timing (renderers, any thread): one command buffer's GPU interval in ns of the renderer's
// GPU clock. Reported only while timing_wanted() (renderers add no timing work otherwise).
void gpu_interval(double startNs, double endNs);
// whether the renderers time their command buffers: dynamic resolution on, or the GPU watched (watch_gpu)
bool timing_wanted();
// (VR, docs/VR_STREAMING.md) the GPU's busy time watched without dynamic resolution: a VR session logs it every 10 s
// (a streamed headset's video encoder shares the GPU). take_watched_ns: the busy ns since the last take
void watch_gpu(bool on);
double take_watched_ns();

// render thread, once per frame at the frame boundary (the renderers' latch_res_scale): decides and
// returns the factor for the next frame. `base` is the internal resolution (for the log and report).
float latch(uint64_t frame, float base);
float factor();  // the factor in effect (any thread)
// the fixed factor while dynamic resolution is off (dynres.cpp): the internal resolution's 0.5x / 0.75x from the
// Graphics menu, by default 0.75 on a Quest or Steam Frame, else 1 (WWHD_RES_FIXED overrides). Safe from static
// initialisation: the renderers start at it, so their first targets are not made again at the first latch
float fixed_factor();
void set_fixed(float factor);   // 1 (or more): none; else the nearest level
// (docs/VR_DLSS.md, stage D4) a DLSS upscaling mode's internal resolution: while below 1 it is the fixed factor,
// whatever the menu's (set_fixed) says (the settings load after the DLSS probe and would set the menu's back)
void set_dlss(float factor);
bool fixed_applies_at_start();  // a Quest or Frame: a new choice is drawn from the next start

// The renderers keep the render target images of the levels next to the current one and make them ahead
// while on (a few a frame): a one-level scale change then only copies. Released when off.
int levels();        // levels in use: 1 + the lowest level (1 while off)
float level_factor(int level);
constexpr int kPrecreatePerFrame = 4;  // images made ahead per frame (spreads the allocation cost)

// the performance report's line ("Dynamic resolution: ..."), without newline
std::string status();

// counters for tests and the report
struct Stats {
    uint64_t changes = 0;   // scale changes
    uint64_t reused = 0;    // render target images taken from the kept ones at a change
    uint64_t created = 0;   // render target images created at a change
};
Stats stats();
void count_reused();
void count_created();
// render thread: time spent at a scale change (logged with WWHD_DYNRES_LOG_FRAMES, per frame)
void note_rescale(bool created, double createMs, double copyMs);
void note_precreated(int images, double ms);
void note_view();   // an image view was created (render thread)
void note_image();  // any image was created (render thread; render targets, mip chains, AO, feedback copies)

}  // namespace dynres

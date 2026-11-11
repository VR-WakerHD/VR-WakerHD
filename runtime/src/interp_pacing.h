// Frame interpolation math without game state: blend fractions, the pass schedule of a logic step,
// paced decisions and the display cap. interp.cpp and interp_fx.cpp use it; runtime/tools/
// interp_pacing_test.cpp tests it (ctest interp_pacing).
//
// A logic step S -> S+1 is drawn in n + 1 passes: the logic pass (phase 0) and hold passes 1..n.
// Hold passes 1..n-1 are blended like the logic pass; hold pass n, the record pass, draws S+1
// exactly and records it as the "before" of the next step's blended frames. n is the step's number
// of in-between frames: fps / 30 - 1 at most (1 at 60 fps, 3 at 120, 7 at 240).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace interp {
namespace pacing {

// Blends at fraction t (0 = the previous step, 1 = the new one). At t = 1/2 they keep the original
// halfway arithmetic, so 60 fps draws bit-identical frames to the halfway-only code.
// (Other fractions as (1-t) a + t b: exact at both ends, unlike a + (b - a) t.)
inline float lerp_f(float a, float b, float t) { return t == 0.5f ? 0.5f * (a + b) : (1.0f - t) * a + t * b; }
// s16 angle from a towards b, the short way round
inline int16_t lerp_s16(int16_t a, int16_t b, float t) {
    const int16_t d = (int16_t)(b - a);
    return t == 0.5f ? (int16_t)(a + d / 2) : (int16_t)(a + (int)std::lround(d * t));
}
inline uint8_t lerp_u8(uint8_t a, uint8_t b, float t) {
    return t == 0.5f ? (uint8_t)((a + b + 1) / 2) : (uint8_t)std::lround(a + (b - a) * t);
}
// Weights of two unit vectors / quaternions for a blend at t whose cosine is cosang (>= 0 after
// choosing the short way): slerp (constant angular speed over the step's frames; nlerp would bunch
// the in-between frames towards the middle). At t = 1/2 both weights are 1 (nlerp, exact there, and
// the original arithmetic); the caller normalises.
inline void slerp_weights(float cosang, float t, float& wa, float& wb) {
    if (t == 0.5f) {
        wa = wb = 1.0f;
        return;
    }
    const float ang = std::acos(std::min(1.0f, std::max(-1.0f, cosang)));
    if (ang > 1e-4f) {
        const float s = std::sin(ang);
        wa = std::sin((1 - t) * ang) / s;
        wb = std::sin(t * ang) / s;
    } else {
        wa = 1 - t;
        wb = t;
    }
}

// Blend fraction of a pass. phase 0 = logic pass, k = hold pass k; n = the step's in-between
// frames; exact = the step is drawn without blending (no record pass before it).
inline float pass_fraction(int phase, int n, bool exact) {
    if (exact) return 1.0f;
    if (phase == 0) return 1.0f / float(n + 1);
    return phase < n ? float(phase + 1) / float(n + 1) : 1.0f;
}

// Frame rates frame interpolation offers: 40, 60, 120, 240 (anything else rounds down to one of
// them; below 40, 60)
inline int valid_fps(int f) { return f >= 240 ? 240 : f >= 120 ? 120 : f >= 60 ? 60 : f >= 40 ? 40 : 60; }

// 40 fps: 3 logic steps over 4 frames (a frame every 1.5 vsyncs of 59.94 Hz, 25.03 ms). In each
// group of 3 steps one step gets an in-between frame (its logic pass blended, its record pass
// exact) and the other two get one frame each: their logic pass, blended and also recorded as the
// "before" of the next step's frames (a blended record pass, Fractions40). Which step gets the
// in-between frame follows from the step slots on the virtual vsync (gx2/vsync_pacing.h
// step_frames: the step whose next step's frame is two frames later). Without a slot (Keep game
// speed off, or before the first slot) the first step of every three.
inline bool is_fps40(int fps) { return fps == 40; }
inline int in_between_40(uint64_t step) { return step % 3 == 0 ? 1 : 0; }
// 40 fps: the fraction each frame is drawn at, so that every frame shows the game at its own time,
// 3/4 of a step after the previous frame (issue #254). The frames are 1.5 vsyncs apart and the steps
// 2, so a step's logic frame comes 0, 1 or 0.5 vsyncs after its slot (the step with the in-between
// frame, the next one, the one after: its slot is a granule boundary plus the grid's phase, the
// same for all three). Drawn a step late less those offsets: the step with the in-between frame at
// 1/4 (its record pass, a frame later, exactly at its step), the next step at 3/4, the one after at
// 1/2, then 1/4 again (game time +0.75 step per frame). Before, the step with the in-between frame
// was drawn at 1/2 and the others exactly: +0.5, +0.5, +1, +1 step per frame (camera turns sped
// up and slowed down 10 times a second).
// A step without an in-between frame is drawn 1/4 step earlier in its step than the frame before it
// was in its own (3/4 after the record pass's exact frame), at least at 1/4 (the slots' order
// broken by a late step: two such steps after a record pass, or one after another blended record
// pass at 1/4, keep 1/4); an exact step (no record pass before it) at 1.
struct Fractions40 {
    float last = 1.0f;  // where the last frame was drawn in its step (1: exactly)
    // the step the frames fall behind by each frame: 1/4 at 40 fps (frames 3/4 of a step apart), 1/6 at 36 fps (5/6)
    float lag = 0.25f;
    // the logic pass of a step with n in-between frames (0 or 1); exact: drawn exactly
    float logic(int n, bool exact) {
        const float t = exact ? 1.0f : n > 0 ? lag : std::max(lag, last - lag);
        last = t;
        return t;
    }
    void record() { last = 1.0f; }  // the step's record pass drew it exactly
};
// A blended record pass (40 fps) records the exact state of its step while it draws the step
// blended: a record kept per object (a map from key to record) is replaced by the new one, stamped
// for the next pass, and this pass's blends go on using the one it replaced (kept in `before`,
// which the caller clears at every pass start: an object drawn twice in the pass blends both draws
// from the same "before"). Returns the record to blend from (nullptr: none).
template <class Map, class StampOf>
const typename Map::mapped_type* replace_record(Map& map, Map& before, const typename Map::key_type& key,
                                                typename Map::mapped_type rec, uint64_t stamp, StampOf stampOf) {
    auto it = map.find(key);
    if (it != map.end()) {
        if (stampOf(it->second) != stamp) before[key] = std::move(it->second);  // (its first record in this pass)
        it->second = std::move(rec);
    } else {
        map.emplace(key, std::move(rec));
    }
    auto b = before.find(key);
    return b != before.end() ? &b->second : nullptr;
}
// the drawn frame interval at 40 fps: 3/4 of a step (1.5 vsyncs), in the step's unit
inline int64_t frame_40(int64_t step) { return step * 3 / 4; }

// 36 fps (VR, a 72 Hz headset at half its rate, 2026-11-03: every frame shown for two refreshes; docs/VR_FRAME_RATE.md):
// 5 logic steps over 6 frames, each frame 5/6 of a step after the last, as 40 fps is 3 over 4. The step slots on the
// virtual vsync (granule 20 ticks, steps of 24) give one step in five 2 frames and the others 1 (in_between_36). As at
// 40 fps (Fractions40 with a lag of 1/6): the step with the in-between frame has its logic pass at 1/6 and its record
// pass exact; the next four steps are blended record passes at 5/6, 4/6, 3/6 and 2/6; then 1/6 again (game time +5/6
// of a step a frame). The frames show the game evenly; the headset shows each for two refreshes.
inline bool is_fps36(int fps) { return fps == 36; }
inline int in_between_36(uint64_t step) { return step % 5 == 0 ? 1 : 0; }  // (without a step slot)
// the drawn frame interval at 36 fps: 5/6 of a step (1 2/3 vsyncs of 59.94 Hz), in the step's unit
inline int64_t frame_36(int64_t step) { return step * 5 / 6; }
constexpr float kLag36 = 1.0f / 6.0f, kLag40 = 0.25f;

// 45 fps (VR, a 90 Hz headset: every frame shown for two refreshes; docs/VR_FRAME_RATE.md): 2 logic steps over 3
// frames, each frame 2/3 of a step after the last, so the game moves evenly from frame to frame. The step slots on
// the virtual vsync (granule 16 ticks, steps of 24) give the steps 2 and 1 frames in turn:
// - a step with 2 frames: its logic pass drawn at 1/3 of the step, its record pass exact (as at 60 fps, the logic
//   pass at a third instead of halfway);
// - a step with 1 frame: its logic pass drawn at 2/3 of the step and recorded at the same time (a blended record
//   pass: drawn blended, its exact state kept as the "before" of the next step's frames, interp.cpp).
// The frames show the game at 1/3, 1, 1 2/3, 2 1/3, 3, ... steps. A step without a record pass before it (one that
// was dropped) is drawn exactly and recorded, as at 40 fps.
inline bool is_fps45(int fps) { return fps == 45; }
// 72 and 80 fps (VR on a streamed headset at 72 or 80 Hz only, 2026-11-05: docs/VR_STREAMING.md): a frame on every
// refresh. The streaming runtime encodes and sends the frames it gets; at 72 Hz the game drew 60 fps, a frame missing
// on every sixth refresh, and over Wi-Fi a player saw lag that other games did not have. 72 fps is 12 frames over 5
// logic steps, 80 fps 8 over 3: steps of 3 and 2 frames (3,2,3,2,2 and 3,3,2), each step ending on its exact record
// pass as at 60 and 90 fps (the same passes and records: pass_fraction), its frames evenly within it (thirds or
// halves of a step). The step slots on the virtual vsync (granules of 10 and 9 ticks, steps of 24) say which steps get
// three. Every frame is one refresh apart; the game's time moves a third or a half of a step a frame (not 5/12 and 3/8
// exactly: steps cannot end between frames on these passes)
inline bool is_fps_variable(int fps) { return fps == 72 || fps == 80; }
// frames a step without a step slot: the pattern above, from a step counter
inline int variable_step_frames(int fps, uint64_t step) {
    if (fps == 80) return step % 3 == 2 ? 2 : 3;
    const uint64_t k = step % 5;
    return k == 0 || k == 2 ? 3 : 2;
}
// the drawn frame interval: 5/12 of a step at 72 fps, 3/8 at 80, in the step's unit
inline int64_t frame_variable(int fps, int64_t step) { return fps == 80 ? step * 3 / 8 : step * 5 / 12; }
// the virtual vsync's ticks a frame (a step is 24): 10 at 72 fps, 9 at 80
inline uint32_t granule_variable(int fps) { return fps == 80 ? 9u : 10u; }
// frames a step on average: 2.4 and 8/3
inline double frames_per_step_variable(int fps) { return fps == 80 ? 8.0 / 3.0 : 2.4; }
inline int in_between_45(uint64_t step) { return step % 2 == 0 ? 1 : 0; }  // (without a step slot)
// the drawn frame interval at 45 fps: 2/3 of a step (1 1/3 vsyncs of 59.94 Hz), in the step's unit
inline int64_t frame_45(int64_t step) { return step * 2 / 3; }
// the blend fraction of a pass at 45 fps: phase 0 = logic pass, 1 = the record pass of a step with 2 frames;
// n = the step's in-between frames (1 or 0); exact = no record pass before it
inline float pass_fraction_45(int phase, int n, bool exact) {
    if (exact) return 1.0f;
    if (phase > 0) return 1.0f;
    return n >= 1 ? 1.0f / 3.0f : 2.0f / 3.0f;
}

// VR frame-rate modes (vr.frameRate, docs/VR_FRAME_RATE.md), by meaning, the rate following the headset's refresh rate
// hz (the one the runtime runs, not the one asked for):
// - kVrUnlocked: as many frames as the headset shows evenly and the computer draws (cap_fps_vr: 90 fps on a 90 Hz
//   headset, 120 on 120 Hz; two frames a step when three do not fit);
// - kVrFull: capped at the refresh rate (90 fps at 90 Hz, 120 at 120 Hz, 60 at 60 Hz);
// - kVrHalf: capped at half of it (45 fps at 90 Hz, 60 at 120 Hz, 40 at 80 Hz, 30 at 60 Hz);
// - kVrAuto: capped at the refresh rate while its frames fit, else at half (auto_rate_step below): the rates the
//   headset shows evenly (90 or 45 at 90 Hz, 120 or 60 at 120 Hz) instead of a mix of three and two frames a step when
//   three do not quite fit;
// - kVrOriginal: the game's own 30 fps (settings from before the half cap; not offered any more).
// A cap the pacer has no schedule for (72 and 80 fps: vr_drawable_fps) draws the fastest one below it. 36 fps (half of
// 72 Hz) has its own schedule (is_fps36).
// The game logic runs at 30 steps a second in every mode.
enum VrFrameMode : int { kVrUnlocked = 0, kVrHalf = 1, kVrOriginal = 2, kVrAuto = 3, kVrFull = 4 };
constexpr int kVrLastMode = kVrFull;
// the cap a mode asks for at refresh rate hz (rounded, 0: unknown): 0 for unlocked and auto (no cap of their own)
inline int vr_mode_cap(int mode, int hz) {
    if (mode == kVrOriginal) return 30;
    if (hz <= 0) return 0;
    if (mode == kVrFull) return hz;
    if (mode == kVrHalf) return hz / 2;
    return 0;
}
// the frame rates the pacer has schedules for in VR (whole frames a logic step, or the 40 and 45 fps patterns; 30 is
// frame interpolation off), and the fastest of them within a cap
// streamed: a streamed headset (docs/VR_STREAMING.md), where 80 and 72 fps have schedules too (is_fps_variable)
inline int vr_drawable_fps(int cap, bool streamed = false) {
    for (int r : {120, 90, 80, 72, 60, 45, 40, 36})
        if (r <= cap && (streamed || !is_fps_variable(r))) return r;
    return 30;
}

// kVrAuto's choice, one call per step drawn (game thread). At the unlocked rate it counts the in-between frames the
// pacer drew of those it could (a step's capacity): under kAutoDrop of them over kAutoWindow steps (3 s), it goes to
// half. From half it tries unlocked again after `backoff` steps (10 s at first) for kAutoTrial steps (2 s, after
// kAutoWarmup steps the pacer's estimates settle in) and keeps it with kAutoKeep of the frames drawn; a failed try
// doubles the wait, up to a minute. The two shares apart keep it
// from switching back and forth, the waits keep failed tries rare (each one shows the uneven rate for 2 s).
// (Desktop stereo, Linux, the Great Sea: unlocked drew 56 to 77 % of its in-between frames, half every frame.)
struct AutoRate {
    bool half = false;   // the next steps run at half the rate
    bool trial = false;  // unlocked again, on trial
    int64_t steps = 0, tryAt = 0, backoff = 300;  // (kAutoBackoff, below)
    int windowSteps = 0, windowDrawn = 0, windowPossible = 0, warmup = 0;
    double lastShare = -1;  // the share of the last window judged (for the log)
    bool streamed = false;  // a streamed headset (docs/VR_STREAMING.md): the waits below are kAutoBackoffStreamed's
    // Auto's two profiles (asked for, 2026-11-05: "auto mode has a wired optimal mode different than the streamed
    // optimal one"): wired optimal (false: kAutoWindow, kAutoDrop, kAutoKeep below) and streamed optimal (a streamed
    // headset with Streaming optimal: kAutoWindowStreamed, kAutoDropStreamed, kAutoKeepStreamed)
    bool streamOptimal = false;
};
constexpr int kAutoWindow = 45, kAutoTrial = 60, kAutoWarmup = 15;
constexpr int64_t kAutoBackoff = 300, kAutoBackoffMax = 1200;  // (steps: 10 s, at most 40 s)
static_assert(AutoRate{}.backoff == kAutoBackoff, "AutoRate starts with kAutoBackoff");
// a streamed headset (Virtual Desktop, a Quest on a PC: docs/VR_STREAMING.md) holds half the rate longer once there:
// 40 s before the full rate is tried again, at most 160 s. Each try and return is a change of smoothness, and its own
// frame smoothing (Virtual Desktop's SSW) also works at half the rate (a Quest through VDXR on Wi-Fi: Auto went between
// 90 and 45 again and again, and the player saw judder)
constexpr int64_t kAutoBackoffStreamed = 1200, kAutoBackoffStreamedMax = 4800;  // (steps: 40 s, at most 160 s)
// (VR fork: favours the full rate. It was strict, 97 % to stay and 99 % to come back: each frame unlocked misses is a
// 22 ms gap in 11 ms ones, about six a second at 90 %, which the headset shows as judder; in busy scenes it then sat at
// 45 fps most of the time (unlocked drew 56 to 68 % there). It then stayed while 85 % were drawn and came back at 90 %,
// trying again after 5 s (at most 20 s). In the headset that sat at 60 too often: the frames that do not fit leave two a
// step, 60 fps, shown unevenly on a 90 Hz headset (2026-10-31, asked to favour 90 less). Now it stays while 95 % are
// drawn, judged over 1.5 s, comes back at 98 %, and tries again after 10 s (at most 40 s): more of busy scenes at an
// even 45). Then (2026-10-31, asked for more of 90 in the headset) it stays while 90 % are drawn; the rest as before
constexpr double kAutoDrop = 0.90, kAutoKeep = 0.98;
// Streamed optimal (docs/VR_STREAMING.md, "Auto Mode: wired optimal and streamed optimal"): a streamed headset with
// Streaming optimal. Its full rate is a frame on every refresh (72 and 80 fps included: is_fps_variable), and what the
// stream shows worst is a change of rate (the encoder's cadence and the streaming app's own frame smoothing move with
// it), more than a few frames dropped at the full rate. So it judges over 6 s (not 1.5), so that a hitch (a load, a
// shader) does not change the rate; goes to half below 85 % of the in-between frames drawn (not 90); comes back at 98 %
// as wired does; and waits kAutoBackoffStreamed's times between tries (above)
constexpr int kAutoWindowStreamed = 180;
constexpr double kAutoDropStreamed = 0.85, kAutoKeepStreamed = 0.98;
// drawn of possible: the step's in-between frames (ignored at half). Returns whether the next step runs at half
inline bool auto_rate_step(AutoRate& a, int drawn, int possible) {
    ++a.steps;
    if (a.half) {
        if (a.steps >= a.tryAt) {
            a.half = false;
            a.trial = true;
            a.warmup = kAutoWarmup;
            a.windowSteps = a.windowDrawn = a.windowPossible = 0;
        }
        return a.half;
    }
    if (a.warmup > 0) {
        --a.warmup;
        return false;
    }
    a.windowSteps++;
    a.windowDrawn += drawn;
    a.windowPossible += possible;
    if (a.windowSteps < (a.trial ? kAutoTrial : a.streamOptimal ? kAutoWindowStreamed : kAutoWindow)) return false;
    const double share = a.windowPossible > 0 ? double(a.windowDrawn) / a.windowPossible : 1.0;
    a.lastShare = share;
    const double keep = a.streamOptimal ? kAutoKeepStreamed : kAutoKeep;
    const double drop = a.streamOptimal ? kAutoDropStreamed : kAutoDrop;
    if (share < (a.trial ? keep : drop)) {
        if (a.streamed)
            a.backoff = a.trial ? std::min(2 * std::max(a.backoff, kAutoBackoffStreamed), kAutoBackoffStreamedMax)
                                : std::max(a.backoff, kAutoBackoffStreamed);
        else if (a.trial)
            a.backoff = std::min(2 * a.backoff, kAutoBackoffMax);
        a.half = true;
        a.tryAt = a.steps + a.backoff;
    } else if (a.trial) {
        a.backoff = a.streamed ? kAutoBackoffStreamed : kAutoBackoff;
    }
    a.trial = false;
    a.windowSteps = a.windowDrawn = a.windowPossible = 0;
    return a.half;
}
// (docs/VR_STREAMING.md, "Auto judged in gameplay only") Auto judges only gameplay: the title, the file select, loading
// and the shader preparation at start draw unevenly for reasons the frame rate cannot change (two Quest 3 / Virtual
// Desktop Wi-Fi logs: half the rate chosen at the title, unlocked drawing 38 % and 79 % there behind a 92 ms hitch, then
// held at 45 fps all session on an RTX 5080 busy 5 % of the time). Coming back into gameplay at the unlocked rate it
// waits kAutoSettle steps (3 s: the scene's first draws, its shaders) before a window is judged
constexpr int kAutoSettle = 90;
inline void auto_rate_settle(AutoRate& a) {
    if (a.half) return;
    a.warmup = std::max(a.warmup, kAutoSettle);
    a.windowSteps = a.windowDrawn = a.windowPossible = 0;
}
// The display shows a frame rate evenly when its refresh rate is a whole multiple of it (40 fps on
// 40, 80, 120 or 160 Hz; 60 fps on 60, 120, 240 Hz). hz 0: unknown (taken as even). Rounded rates
// count (119.88 Hz, 59.94 Hz).
inline bool even_on_display(int fps, int hz) {
    if (hz <= 0 || fps <= 0) return true;
    if (hz < fps - 1) return false;
    const int r = hz % fps;
    return r <= 1 || r >= fps - 1;
}

// The frame rate drawn for a chosen one on a display with refresh rate hz (0: unknown) when
// presenting waits for the display's vsync: the highest offered rate the display can show (a
// 144 Hz display gets 120, a 90 Hz one 60), never below 60. Without vsync, or with the rate unknown,
// the chosen rate.
// VR: the headset (xrWaitFrame) paces the frames. 90 Hz and faster headsets get up to 120 fps interpolation
// (paced: in-between frames that do not fit the step are skipped, the 30 Hz logic keeps its speed), slower
// ones up to 60; 40 fps stays as chosen
// In a VR headset frames are shown at the headset's refresh rate (xrWaitFrame paces them), so the drawn rate must
// divide it evenly: 120 fps on a 90 Hz headset showed in-between frames of a 120 fps schedule (a quarter step
// each) at 90 Hz, so the game advanced unevenly from frame to frame (judder; docs/VR_PERFORMANCE.md). 90 fps is
// three frames per logic step (thirds of a step), one per 90 Hz refresh.
// (a streamed headset at 72 or 80 Hz: 72 or 80 fps, a frame every refresh, is_fps_variable)
inline int cap_fps_vr(int fps, int hz, bool streamed = false) {
    fps = valid_fps(fps);
    if (hz <= 0 || fps == 40) return fps;
    if (hz >= 118) return std::min(fps, 120);
    if (hz >= 88) return fps >= 90 ? 90 : fps;
    if (streamed && fps >= 60) {
        if (hz >= 79 && hz <= 81) return 80;
        if (hz >= 71 && hz <= 73) return 72;
    }
    return std::min(fps, 60);
}
// Half the headset's rate with no schedule for it (54 fps at 108 Hz, 72 at 144): the fastest drawable rate under it was
// shown unevenly (45 fps at 108 Hz: frames held 2 and 3 refreshes in turn, judder in every head turn and pan; Steam
// Frame log, 2026-11-03). Now the fastest drawable rate that the refresh rate shows evenly is taken instead, when it
// keeps at least kEvenKeep of that rate (36 fps at 108 Hz, 3 refreshes each: 80 % of 45); further down (36 of 60 at
// 144 Hz) the uneven rate stays, as at the full rate (more frames over evenness there: logged). 0: none fits
inline constexpr double kEvenKeep = 0.75;
inline int vr_even_fps(int cap, int hz) {
    if (hz <= 0) return 0;
    const int fastest = vr_drawable_fps(cap);  // (half the rate: 36 and 40 fps, never the streamed 72 and 80)
    for (int r : {120, 90, 60, 45, 40, 36})
        if (r <= cap && even_on_display(r, hz) && double(r) >= kEvenKeep * double(fastest)) return r;
    return 0;
}
// the rate drawn in a VR mode (VrFrameMode above): a capped mode's cap, as drawable (vr_drawable_fps), at half the rate
// one the refresh rate shows evenly where there is one near it (vr_even_fps); unlocked (and auto, resolved to unlocked
// or half before this) cap_fps_vr; 30 means frame interpolation off
inline int cap_fps_vr_mode(int fps, int hz, int mode, bool streamed = false) {
    if (const int cap = vr_mode_cap(mode, hz)) {
        if (mode == kVrHalf)
            if (const int even = vr_even_fps(cap, hz)) return even;
        return vr_drawable_fps(cap, streamed && mode == kVrFull);
    }
    return cap_fps_vr(fps, hz, streamed);
}
inline int cap_fps(int fps, int hz, bool vsync) {
    fps = valid_fps(fps);
    if (!vsync || hz <= 0) return fps;
    if (is_fps40(fps)) return fps;  // 40 fps is drawn on any display (even_on_display says if it looks even)
    const int shown = hz >= 238 ? 240 : hz >= 118 ? 120 : 60;  // (119.88 Hz and 239.76 Hz modes count)
    return std::min(fps, shown);
}

// Paced interpolation: how many in-between frames the logic pass plans for its step: as many hold
// passes as fit into the step's budget after the logic pass, at the recent durations of each kind
// (they differ: a logic pass runs the game, a hold pass only draws; one average for both kept the
// hold passes out for good once a slow logic pass alone filled the budget). At least one, at most
// n_max. All times in one unit (and a pass never takes less than one vsync of its rate: the
// measured durations include the wait for it).
inline int plan_in_between(int n_max, int64_t budget, int64_t logic_pass, int64_t hold_pass) {
    n_max = std::max(1, n_max);
    hold_pass = std::max<int64_t>(hold_pass, 1);
    const int64_t fit = (budget - std::max<int64_t>(logic_pass, 0)) / hold_pass;
    return int(std::clamp<int64_t>(fit, 1, n_max));
}

// Even frame delivery: the in-between frames a step may have so that its frames divide the display's
// refreshes per step evenly (refreshes: whole refreshes in a 33.37 ms step, 4 at 120 Hz, 2 at 60 Hz,
// 8 at 240 Hz; 0: unknown). On a 120 Hz screen 3 frames a step (90 fps) would stay 1, 1 and 2
// refreshes: uneven by construction. The most of at most n that divide evenly, at least 1.
inline int even_in_between(int n, int refreshes) {
    if (refreshes <= 0) return std::max(1, n);
    for (int k = std::max(1, n); k > 1; --k)
        if (refreshes % (k + 1) == 0) return k;
    return 1;
}

// The first exact step normally needs only its record pass (at 40 fps the
// logic frame itself records). After Fast Forward, fill the available slots
// with exact frames instead: omitting them leaves a 50 ms gap at 40 fps or
// three frame intervals at 120 fps before interpolation has history again.
inline int exact_step_holds(int planned, bool forty, bool resuming) {
    return resuming ? planned : forty ? 0 : 1;
}

// The running average of a pass duration (3/4 old, 1/4 new). A single sample counts at most twice
// the average: one hitch (a shader compiled, a scene loaded) would otherwise make in-between passes
// look unaffordable, and while none are drawn none are measured (measured: one 76 ms pass kept
// them all out for 10 s). A lasting change still gets through within a few passes.
inline int64_t update_average(int64_t avg, int64_t sample) {
    if (avg > 0) sample = std::min(sample, 2 * avg);
    return (avg * 3 + sample) / 4;
}

// The in-between decision of a step at 60 fps (one in-between frame), taken after the logic pass
// (phase 0). The decision comes when the game has issued the logic frame, before the pass waits for its vsync and for the renderer to show it: a render thread that
// delays the flips (issue #178: 15 ms a frame on top) makes the logic pass last longer than the
// decision sees, and counted from the decision every in-between frame "fitted" and each step took
// two render frames (24 instead of 29.97 steps a second). So the step's elapsed time counts the
// logic pass at least at its recent duration (from one pass start to the next), and the in-between
// frame is drawn when the next step then starts at most one drawn frame late (`frame`): that step
// counts its lateness from the step schedule and skips its own in-between frame if it would not
// make up for it. A computer that draws two frames a step a little too slowly so draws as many as
// keep the game at its speed (a third of them skipped at 20 ms a frame) instead of none.
// All times in one unit; pass_start: the logic pass's start (after any wait for its time),
// step_start: the step's start on the schedule. On time and fast, the step's passes take one vsync
// each and the frame is drawn.
inline int64_t logic_pass_elapsed(int64_t now, int64_t pass_start, int64_t step_start, int64_t logic_pass) {
    return std::max(now, pass_start + std::max<int64_t>(logic_pass, 0)) - step_start;
}
inline int64_t one_frame_budget(int64_t step, int64_t frame) { return step + frame; }
// The same rule at 40 fps for a step with an in-between frame: its two frames span two drawn frames
// (50 ms; the next step's frame is due two frames after its own), and the next step may start up to
// a frame late. (At 60 fps the two frames are the step.) A device whose logic passes alone take a
// step or longer (logic_pass: their recent duration) cannot keep 30 steps a second with one frame a
// step: none (0). Without that, each time its late steps started a new schedule (two steps late) the
// next in-between frame fitted the fresh budget of three frames and cost the game another frame
// (simulated, 36 ms passes: 24.6 instead of 27.5 steps a second; at 60 fps the budget of 50 ms
// never fits two such passes).
// A probe (probe_estimate: a while without in-between frames) is let through even so when in-between passes
// (hold_pass) take less than a step. Without in-between frames a logic pass is measured from one step to the next, a
// whole step: after one hitch (a scene's load) took the average to a step, no frame fitted again and the game stayed
// at 30 fps for good (45 fps mode on Linux: logic passes "35 ms", in-between passes 22 ms). The probe's frame ends the
// logic pass before it at its own length, which brings the average back. A device whose in-between passes take a step
// or longer keeps no frames, unprobed (they would only cost it steps).
inline int64_t one_frame_budget_40(int64_t frame, int64_t logic_pass, int64_t step, bool probe = false,
                                   int64_t hold_pass = INT64_MAX) {
    return logic_pass >= step && !(probe && hold_pass < step) ? 0 : one_frame_budget(2 * frame, frame);
}
// After a dropped in-between frame at 60 fps the next logic pass starts at once instead of waiting
// for its step's time (one_frame: a step with one in-between frame at 60 fps). Its lateness on the
// step schedule goes below zero by what the skip saved beyond the deficit so far (a lead:
// slot_step_start kKeep with wait = false), and the following steps count it. The step lateness so
// is the accumulated deficit of the drawn steps (logic pass + in-between pass - step), each skip
// takes a whole in-between pass off it, and the skips come at the deficit rate (deficit / in-between
// pass), one whenever the accumulated deficit reaches a frame: as few as keep the game's speed, and
// evenly spaced. (Waiting threw away the part of a skip the deficit had not used up: a skip that
// one slow pass brought forward saved less than a frame and the next skip came sooner, and a device
// whose passes take longer than a vsync, where the lead after a skip is what lets the next step's
// in-between frame fit, drew almost none.) The lead never moves a logic frame before its slot
// (gx2_core.cpp: the step slot gates its flip). At 120/240 fps a dropped step (not even its record
// pass fitted) still waits.
inline bool wait_after_drop(bool one_frame) { return !one_frame; }

// What follows a pass of a step (paced): phase just ended (0 = logic pass), n = the step's planned
// in-between frames, elapsed = time since the logic pass started, pass = recent duration of a hold
// pass.
enum class Next {
    kStepDone,    // the record pass ended the step: the next pass is a logic pass
    kHold,        // the next hold pass of the step
    kRecord,      // skip the remaining blended hold passes: the next pass is the record pass
    kDrop,        // no pass fits: the rest of the step is dropped, the next logic pass waits for its time
                  // (at 60 fps it starts at once: wait_after_drop)
};
inline Next next_pass(int phase, int n, int64_t elapsed, int64_t pass, int64_t budget) {
    if (phase >= n) return Next::kStepDone;
    const int remaining = n - phase;  // blended hold passes left + the record pass
    if (elapsed + pass * remaining <= budget) return Next::kHold;
    // The record pass goes first: without it the next step has no "before" and is drawn exactly
    // (its logic pass and record pass would then show the same picture twice).
    if (remaining > 1 && elapsed + pass <= budget) return Next::kRecord;
    return Next::kDrop;
}

// A probe of the in-between pass cost (after a long time without in-between passes, and the first
// in-between pass of all) replaces the estimate: the scene may be lighter now. One that hit a hitch
// (more than twice the estimate) counts twice the estimate and asks for another probe soon
// (retry): at the start the first in-between pass met one (90-200 ms, measured here and in the
// #164 report) and kept in-between frames out until the next probe 10 s later.
inline int64_t probe_estimate(int64_t avg, int64_t pass, bool& retry) {
    retry = avg > 0 && pass > 2 * avg;
    return retry ? 2 * avg : pass;
}

// Next::kRecord (not all passes left fit): the phase the step goes on from, so that the next hold
// pass is the one after it. As few blended passes are skipped as make the rest fit (elapsed, pass,
// budget as for next_pass), at least the record pass is left. (Skipping every blended pass up to
// the record pass left gaps of 15-25 ms at 240 fps where in-between passes take a little longer than
// a frame, a few times a second: issue #164.)
inline int skip_to(int phase, int n, int64_t elapsed, int64_t pass, int64_t budget) {
    pass = std::max<int64_t>(pass, 1);
    const int64_t fit = std::clamp<int64_t>((budget - elapsed) / pass, 1, std::max(1, n - phase));
    return n - int(fit);
}

// Paced logic pass at 120/240 fps with a step slot (gx2/vsync_pacing.h: when the step's logic frame
// is due on the virtual vsync). The pass starts one drawn frame (`frame`) before the slot, as it does
// after a record pass, so its frame makes the slot: it waits for that time if it is early and `wait`
// (the step's passes are done), keeps it if less than a step late (the elapsed time counted from it
// makes the step plan fewer in-between frames, and the game catches up), and starts its own schedule
// when later than that (the caller then drops the slot). `start`: the step's start, which its elapsed time counts from.
// window: how late a step may start and keep the schedule (0: one step; keep_window).
enum class StepStart { kWait, kKeep, kLate };
struct StepPlan {
    int64_t start;
    StepStart how;
};
inline StepPlan slot_step_start(int64_t now, int64_t slot, int64_t frame, int64_t step, bool wait, int64_t window = 0) {
    const int64_t start = slot - frame;
    if (wait && now < start) return {start, StepStart::kWait};
    if (now - start < (window > 0 ? window : step)) return {start, StepStart::kKeep};
    return {now, StepStart::kLate};
}
// How late a logic step may start and keep the step schedule. At 60 fps (one_frame: one in-between
// frame a step) two steps: a step that one slow pass (a hitch of 20-40 ms, a few a minute on a
// phone) left more than a step late catches up by skipping the in-between frames of the next steps,
// as it does from a smaller lateness (each skip makes up a step less a logic pass). With one step
// such a step started a new schedule and the time was lost: the game ran slower (issue #254: a
// phone with 17.2 ms passes held 29.85-29.95 instead of 29.97 steps a second; simulated with a
// 15 ms hitch every 200 passes 29.90, now 29.97). Later than that (a scene load) still starts a new
// schedule: catching up would skip many frames in a row. (Two steps is also where the flip side,
// vsync_pacing.h next_slot, starts a new schedule.) `frame` is unused: 120/240 fps keep one step.
inline int64_t keep_window(int64_t step, int64_t /*frame*/, bool one_frame) { return one_frame ? 2 * step : step; }

// Android's automatic frame interpolation (platform/perf_hint.cpp adapt_60, Keep game speed off):
// interpolation pauses while fewer frames a second than this are drawn for 2 s (the game would run
// slower). 50 for 60 fps and up (unchanged); 5/6 of the rate at 40 fps (33.3: at 50 a 40 fps game
// would always pause).
inline double adapt_slow_fps(int fps) { return is_fps40(fps) ? fps * 5.0 / 6.0 : 50.0; }
// and resumes when a 30 fps frame costs the render thread less than this (ms): room for the frames
// of a step (two at 60 fps: 14 ms; 4/3 at 40 fps: 21 ms)
inline double adapt_resume_ms(int fps) { return is_fps40(fps) ? 21.0 : 14.0; }

// Even frame delivery (on by default on Android; WWHD_INTERP_EVEN_PACING=0/1). The passes of a step
// take different times: a logic pass runs the game, an in-between pass only draws. When the game thread is the bottleneck
// (a Xiaomi 14 with the record worker on: logic pass 19-20 ms, in-between pass 12-14 ms) frames
// were ready 20 and 13 ms apart, and its 120 Hz screen showed a third of the 60 fps frames for 1 or
// 3 refreshes instead of 2. Here a frame that is ready early is held on the CPU side (its present
// call waits; the game and the renderer go on) until one frame interval after the previous frame;
// a frame that is late goes out at once. Nothing feeds back into the game's timing.
// - interval: the frame's intended distance to the previous frame (the step's length over its
//   frames: 16.68 ms at 60 fps); 0 turns the schedule off (30 fps, uncapped, no interpolation).
// - Each slot is the previous frame's time plus the interval less a small leak (1/128 interval):
//   the late kind of frame (the logic frame) then anchors the schedule, and a schedule pushed late
//   by one slow frame comes back to the frames' own times within about a hundred frames instead of
//   keeping that delay for good. In the steady state the intervals alternate between interval -
//   leak and interval + leak (16.55 / 16.81 ms at 60 fps: both two 120 Hz refreshes). A smaller leak
//   keeps the schedule nearer the latest the logic frames come (less jitter on screen, a little
//   more latency); a simulation with a 120 Hz display: 1/32 92%, 1/64 94%, 1/128 95% of the frames
//   on screen for two refreshes (without the schedule 56%).
// - A frame is never held longer than one interval (no more than one frame of added latency): one
//   that would wait longer goes out at once and starts the schedule anew.
struct EvenSchedule {
    int64_t last = 0;      // the previous frame's present time
    int64_t interval = 0;  // its interval
    bool valid = false;
    void reset() { valid = false; }
    int64_t leakDivisor = 128;  // the leak per frame: interval / leakDivisor
    int64_t leak(int64_t frameInterval) const { return frameInterval / std::max<int64_t>(1, leakDivisor); }
    // the present time of a frame ready at `ready`
    int64_t plan(int64_t ready, int64_t frameInterval) {
        interval = frameInterval;
        if (frameInterval <= 0) {
            valid = false;
            return ready;
        }
        int64_t t = ready;
        if (valid) {
            const int64_t slot = last + frameInterval - leak(frameInterval);
            if (slot > ready && slot - ready <= frameInterval) t = slot;
        }
        last = t;
        valid = true;
        return t;
    }
    // the frame planned for `due` went out at `at`. A little late (the presenting thread was busy, a
    // timer woke late) keeps the schedule: counted from the late time, every late wake-up would push
    // the following frames later for good (measured: holds grew to a whole interval). Much later
    // (more than 1/8 interval) or early (the next frame forced it out) counts from `at`.
    void presented(int64_t due, int64_t at) {
        if (valid && (at < due || at - due > interval / 8)) last = at;
    }
};

}  // namespace pacing
}  // namespace interp
